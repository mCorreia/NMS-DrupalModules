<?php
/**
 * @file
 * Drush Make processing classes.
 */

/**
 * The base project class.
 */
class DrushMakeProject {

  /**
   * TRUE if make() has been called, otherwise FALSE.
   */
  protected $made = FALSE;

  /**
   * TRUE if download() method has been called successfully, otherwise FALSE.
   */
  protected $downloaded = NULL;

  /**
   * Download location to use.
   */
  protected $download_location = NULL;

  /**
   * Keep track of instances.
   *
   * @see DrushMakeProject::getInstance()
   */
  protected static $self = array();

  /**
   * Default to overwrite to allow recursive builds to process properly.
   *
   * TODO refactor this to be more selective. Ideally a merge would take place
   * instead of an overwrite.
   */
  protected $overwrite = TRUE;

  /**
   * Set attributes and retrieve project information.
   */
  protected function __construct($project) {
    $project['base_contrib_destination'] = $project['contrib_destination'];
    foreach ($project as $key => $value) {
      $this->{$key} = $value;
    }
  }

  /**
   * Get an instance for the type and project.
   *
   * @param string $type
   *   Type of project: core, library, module, profile, or translation.
   * @param array $project
   *   Project information.
   *
   * @return mixed
   *   An instance for the project or FALSE if invalid type.
   */
  public static function getInstance($type, $project) {
    if (!isset(self::$self[$type][$project['name']])) {
      $class = 'DrushMakeProject_' . $type;
      self::$self[$type][$project['name']] = class_exists($class) ? new $class($project) : FALSE;
    }
    return self::$self[$type][$project['name']];
  }

  /**
   * Download a project.
   */
  function download() {
    $this->downloaded = TRUE;

    // In some cases, make_download_factory() is going to need to know the
    // full version string of the project we're trying to download. However,
    // the version is a project-level attribute, not a download-level
    // attribute. So, if we don't already have a full version string in the
    // download array (e.g. if it was initialized via the release history XML
    // for the PM case), we take the version info from the project-level
    // attribute, convert it into a full version string, and stuff it into
    // $this->download so that the download backend has access to it, too.
    if (!empty($this->version) && empty($this->download['full_version'])) {
      $full_version = '';
      $matches = array();
      // Core needs different conversion rules than contrib.
      if (!empty($this->type) && $this->type == 'core') {
        // Generally, the version for core is already set properly.
        $full_version = $this->version;
        // However, it might just be something like '7' or '7.x', in which
        // case we need to turn that into '7.x-dev';
        if (preg_match('/^\d+(\.x)?$/', $this->version, $matches)) {
          // If there's no '.x' already, append it.
          if (empty($matches[1])) {
            $full_version .= '.x';
          }
          $full_version .= '-dev';
        }
      }
      // Contrib.
      else {
        // If the version doesn't already define a core version, prepend it.
        if (!preg_match('/^\d+\.x-\d+.*$/', $this->version)) {
          // Just find the major version from $this->core so we don't end up
          // with version strings like '7.12-2.0'.
          $core_parts = explode('.', $this->core);
          $full_version = $core_parts[0] . '.x-';
        }
        $full_version .= $this->version;
        // If the project-level version attribute is just a number it's a major
        // version.
        if (preg_match('/^\d+(\.x)?$/', $this->version, $matches)) {
          // If there's no '.x' already, append it.
          if (empty($matches[1])) {
            $full_version .= '.x';
          }
          $full_version .= '-dev';
        }
      }
      $this->download['full_version'] = $full_version;
    }

    if (make_download_factory($this->name, $this->download, $this->download_location) === FALSE) {
      $this->downloaded = FALSE;
    }
    return $this->downloaded;
  }

  /**
   * Build a project.
   */
  function make() {
    if ($this->made) {
      drush_log(dt('Attempt to build project @project more then once prevented.', array('@project' => $this->name)));
      return TRUE;
    }
    $this->made = TRUE;

    if (is_null($this->download_location)) {
      $this->download_location = $this->findDownloadLocation();
    }
    if ($this->download() === FALSE) {
      return FALSE;
    }
    if (!$this->addLockfile($this->download_location)) {
      return FALSE;
    }
    if (!$this->applyPatches($this->download_location)) {
      return FALSE;
    }
    if (!$this->getTranslations($this->download_location)) {
      return FALSE;
    }
    // Handle .info file re-writing (if so desired).
    if (!drush_get_option('no-gitinfofile', FALSE) && isset($this->download['type']) && $this->download['type'] == 'git') {
      $this->processGitInfoFiles();
    }
    if (!$this->recurse($this->download_location)) {
      return FALSE;
    }
    return TRUE;
  }

  /**
   * Determine the location to download project to.
   */
  function findDownloadLocation() {
    $this->path = $this->generatePath();
    $this->project_directory = !empty($this->directory_name) ? $this->directory_name : $this->name;
    $this->download_location = $this->path . '/' . $this->project_directory;
    // This directory shouldn't exist yet -- if it does, stop,
    // unless overwrite has been set to TRUE.
    if (is_dir($this->download_location) && !$this->overwrite) {
      drush_set_error(dt('Directory not empty: !directory', array('!directory' => $this->download_location)));
      return FALSE;
    }
    elseif ($this->download['type'] === 'pm') {
      // pm-download will create the final contrib directory.
      drush_mkdir(dirname($this->download_location));
    }
    else {
      drush_mkdir($this->download_location);
    }
    return $this->download_location;
  }

  /**
   * Retrieve and apply any patches specified by the makefile to this project.
   */
  function applyPatches($project_directory) {
    if (empty($this->patch)) {
      return TRUE;
    }

    $patches_txt = '';
    $ignore_checksums = drush_get_option('ignore-checksums');
    foreach ($this->patch as $info) {
      if (!is_array($info)) {
        $info = array('url' => $info);
      }
      // Download the patch.
      if ($filename = _make_download_file($info['url'])) {
        $patched = FALSE;
        $output = '';
        // Test each patch style; -p1 is the default with git. See
        // http://drupal.org/node/1054616
        $patch_levels = array('-p1', '-p0');
        foreach ($patch_levels as $patch_level) {
          $checked = drush_shell_exec('cd %s && GIT_WORK_TREE=. git apply --check %s %s --verbose', $project_directory, $patch_level, $filename);
          if ($checked) {
            // Apply the first successful style.
            $patched = drush_shell_exec('cd %s && GIT_WORK_TREE=. git apply %s %s --verbose', $project_directory, $patch_level, $filename);
            break;
          }
        }

        // In some rare cases, git will fail to apply a patch, fallback to using
        // the 'patch' command.
        if (!$patched) {
          foreach ($patch_levels as $patch_level) {
            // --no-backup-if-mismatch here is a hack that fixes some
            // differences between how patch works on windows and unix.
            if ($patched = drush_shell_exec("patch %s --no-backup-if-mismatch -d %s < %s", $patch_level, $project_directory, $filename)) {
              break;
            }
          }
        }

        if ($output = drush_shell_exec_output()) {
          // Log any command output, visible only in --verbose or --debug mode.
          drush_log(implode("\n", $output));
        }

        // Set up string placeholders to pass to dt().
        $dt_args = array(
          '@name' => $this->name,
          '@filename' => basename($filename),
        );

        if ($patched) {
          if (!$ignore_checksums && !_make_verify_checksums($info, $filename)) {
             return FALSE;
          }
          $patches_txt .= '- ' . $info['url'] . "\n";
          drush_log(dt('@name patched with @filename.', $dt_args), 'ok');
        }
        else {
          make_error('PATCH_ERROR', dt("Unable to patch @name with @filename.", $dt_args));
        }
        drush_op('unlink', $filename);
      }
      else {
        make_error('DOWNLOAD_ERROR', 'Unable to download ' . $info['url'] . '.');
        return FALSE;
      }
    }
    if (!empty($patches_txt) && !drush_get_option('no-patch-txt') && !file_exists($project_directory . '/PATCHES.txt')) {
      $patches_txt = "The following patches have been applied to this project:\n" .
        $patches_txt .
        "\nThis file was automatically generated by Drush Make (http://drupal.org/project/drush).";
      file_put_contents($project_directory . '/PATCHES.txt', $patches_txt);
      drush_log('Generated PATCHES.txt file for ' . $this->name, 'ok');
    }
    return TRUE;
  }

  /**
   * Process info files when downloading things from git.
   */
  function processGitInfoFiles() {
    // Figure out the proper version string to use based on the .make file.
    // Best case is the .make file author told us directly.
    if (!empty($this->download['full_version'])) {
      $full_version = $this->download['full_version'];
    }
    // Next best is if we have a tag, since those are identical to versions.
    elseif (!empty($this->download['tag'])) {
      $full_version = $this->download['tag'];
    }
    // If we have a branch, append '-dev'.
    elseif (!empty($this->download['branch'])) {
      $full_version = $this->download['branch'] . '-dev';
    }
    // Ugh. Not sure what else we can do in this case.
    elseif (!empty($this->download['revision'])) {
      $full_version = $this->download['revision'];
    }
    // Probably can never reach this case.
    else {
      $full_version = 'unknown';
    }

    // If the version string ends in '.x-dev' do the Git magic to figure out
    // the appropriate 'rebuild version' string, e.g. '7.x-1.2+7-dev'.
    $matches = array();
    if (preg_match('/^(.+).x-dev$/', $full_version, $matches)) {
      require_once dirname(__FILE__) . '/../pm/package_handler/git_drupalorg.inc';
      $full_version = drush_pm_git_drupalorg_compute_rebuild_version($this->download_location, $matches[1]);
    }
    require_once dirname(__FILE__) . '/../pm/pm.drush.inc';
    drush_pm_inject_info_file_metadata($this->download_location, $this->name, $full_version);
  }

  /**
   * Add a lock file.
   */
  function addLockfile($project_directory) {
    if (!empty($this->lock)) {
      file_put_contents($project_directory . '/.drush-lock-update', $this->lock);
    }
    return TRUE;
  }

  /**
   * Retrieve translations for this project.
   */
  function getTranslations($project_directory) {
    static $cache = array();
    $langcodes = $this->translations;
    if ($langcodes && in_array($this->type, array('core', 'module', 'profile', 'theme'), TRUE)) {
      // Support the l10n_path, l10n_url keys from l10n_update. Note that the
      // l10n_server key is not supported.
      if (isset($this->l10n_path)) {
        $update_url = $this->l10n_path;
      }
      else {
        if (isset($this->l10n_url)) {
          $l10n_server = $this->l10n_url;
        }
        else {
          $l10n_server = FALSE;
        }
        if ($l10n_server) {
          if (!isset($cache[$l10n_server])) {
            if ($filename = _make_download_file($l10n_server)) {
              $server_info = simplexml_load_string(file_get_contents($filename));
              $cache[$l10n_server] = !empty($server_info->update_url) ? $server_info->update_url : FALSE;
            }
          }
          if ($cache[$l10n_server]) {
            $update_url = $cache[$l10n_server];
          }
          else {
            make_error('XML_ERROR', dt("Could not retrieve l10n update url for !project.", array('!project' => $project['name'])));
            return FALSE;
          }
        }
      }
      if ($update_url) {
        $failed = array();
        foreach ($langcodes as $langcode) {
          $variables = array(
            '%project' => $this->name,
            '%release' => $this->download['full_version'],
            '%core' => $this->core,
            '%language' => $langcode,
            '%filename' => '%filename',
          );
          $url = strtr($update_url, $variables);

          // Download the translation file.  Since its contents are volatile,
          // cache for only 4 hours.
          if ($filename = _make_download_file($url, 3600 * 4)) {
            // If this is the core project type, download the translation file
            // and place it in every profile and an additional copy in
            // modules/system/translations where it can be detected for import
            // by other non-default install profiles.
            if ($this->type === 'core') {
              $profiles = drush_scan_directory($project_directory . '/profiles', '/.*/', array(), 0, FALSE, 'filename', 0, TRUE);
              foreach ($profiles as $profile) {
                if (is_dir($project_directory . '/profiles/' . $profile->basename)) {
                  drush_mkdir($project_directory . '/profiles/' . $profile->basename . '/translations');
                  drush_copy_dir($filename, $project_directory . '/profiles/' . $profile->basename . '/translations/' . $langcode . '.po');
                }
              }
              drush_mkdir($project_directory . '/modules/system/translations');
              drush_copy_dir($filename, $project_directory . '/modules/system/translations/' . $langcode . '.po');
            }
            else {
              drush_mkdir($project_directory . '/translations');
              drush_copy_dir($filename, $project_directory . '/translations/' . $langcode . '.po', TRUE);
            }
          }
          else {
            $failed[] = $langcode;
          }
        }
        if (empty($failed)) {
          drush_log('All translations downloaded for ' . $this->name, 'ok');
        }
        else {
          drush_log('Unable to download translations for ' . $this->name . ': ' . implode(', ', $failed), 'warning');
        }
      }
    }
    return TRUE;
  }

  /**
   * Generate the proper path for this project type.
   *
   * @param boolean $base
   *   Whether include the base part (tmp dir). Defaults to TRUE.
   */
  protected function generatePath($base = TRUE) {
    $path = array();
    if ($base) {
      $path[] = make_tmp();
      $path[] = '__build__';
    }
    if (!empty($this->contrib_destination)) {
      $path[] = $this->contrib_destination;
    }
    if (!empty($this->subdir)) {
      $path[] = $this->subdir;
    }
    return implode('/', $path);
  }

  /**
   * Return the proper path for dependencies to be placed in.
   *
   * @return string
   *   The path that dependencies will be placed in.
   */
  protected function buildPath($directory) {
    return $this->base_contrib_destination;
  }

  /**
   * Recurse to process additional makefiles that may be found during
   * processing.
   */
  function recurse($path) {
    $makefile = $this->download_location . '/' . $this->name . '.make';
    if (!file_exists($makefile)) {
      $makefile = $this->download_location . '/drupal-org.make';
      if (!file_exists($makefile)) {
        return TRUE;
      }
    }
    drush_log(dt("Found makefile: !makefile", array("!makefile" => basename($makefile))), 'ok');

    $info = make_parse_info_file($makefile);
    if (!($info = make_validate_info_file($info))) {
      return FALSE;
    }
    $build_path = $this->buildPath($this->name);
    make_projects(TRUE, trim($build_path, '/'), $info, $this->build_path);
    make_libraries(trim($build_path, '/'), $info, $this->build_path);

    return TRUE;
  }
}

/**
 * For processing Drupal core projects.
 */
class DrushMakeProject_Core extends DrushMakeProject {
  /**
   * Override constructor for core to adjust project info.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    // subdir and contrib_destination are not allowed for core.
    $this->subdir = '';
    $this->contrib_destination = '';
  }

  /**
   * Determine the location to download project to.
   */
  function findDownloadLocation() {
    $this->path = $this->download_location = $this->generatePath();
    $this->project_directory = '';
    if (is_dir($this->download_location)) {
      drush_set_error(dt('Directory not empty: !directory', array('!directory' => $this->download_location)));
      return FALSE;
    }
    elseif ($this->download['type'] === 'pm') {
      // pm-download will create the final __build__ directory, so nothing to do
      // here.
    }
    else {
      drush_mkdir($this->download_location);
    }
    return $this->download_location;
  }
}

/**
 * For processing libraries.
 */
class DrushMakeProject_Library extends DrushMakeProject {
  /**
   * Override constructor for libraries to properly set contrib destination.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    // Allow libraries to specify where they should live in the build path.
    if (isset($project['destination'])) {
      $project_path = $project['destination'];
    }
    else {
      $project_path = 'libraries';
    }

    $this->contrib_destination = ($this->base_contrib_destination != '.' ? $this->base_contrib_destination . '/' : '') . $project_path;
  }

  /**
   * No recursion for libraries, sorry :-(
   */
  function recurse($path) {}

  /**
   * No translations for libraries.
   */
  function getTranslations($download_location) {}
}

/**
 * For processing modules.
 */
class DrushMakeProject_Module extends DrushMakeProject {
  /**
   * Override constructor for modules to properly set contrib destination.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    $this->contrib_destination = ($this->base_contrib_destination != '.' ? $this->base_contrib_destination . '/' : '') . 'modules';
  }
}

/**
 * For processing installation profiles.
 */
class DrushMakeProject_Profile extends DrushMakeProject {
  /**
   * Override contructor for installation profiles to properly set contrib
   * destination.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    $this->contrib_destination = (!empty($this->destination) ? $this->destination : 'profiles');
  }

  /**
   * Find the build path.
   */
  protected function buildPath($directory) {
    return $this->generatePath(FALSE) . '/' . $directory;
  }
}

/**
 * For processing themes.
 */
class DrushMakeProject_Theme extends DrushMakeProject {
  /**
   * Override contructor for themes to properly set contrib destination.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    $this->contrib_destination = ($this->base_contrib_destination != '.' ? $this->base_contrib_destination . '/' : '') . 'themes';
  }
}

/**
 * For processing translations.
 */
class DrushMakeProject_Translation extends DrushMakeProject {
  /**
   * Override constructor for translations to properly set contrib destination.
   */
  protected function __construct(&$project) {
    parent::__construct($project);
    switch ($project['core']) {
      case '5.x':
        // Don't think there's an automatic place we can put 5.x translations,
        // so we'll toss them in a translations directory in the Drupal root.
        $this->contrib_destination = ($this->base_contrib_destination != '.' ? $this->base_contrib_destination . '/' : '') . 'translations';
        break;

      default:
        $this->contrib_destination = '';
        break;
    }
  }
}
