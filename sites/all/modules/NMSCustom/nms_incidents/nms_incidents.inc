<?php

/**
 * Callback for creating Incident Reports resources.
 * 
 * The method will receive an object describing the incident report
 * that is about to be saved. The atributes we want are 'subject',
 * 'description', 'date', 'location', 'latitude' and 'longitude'.
 * We will throw errors if these atributes are missing.
 * 
 * We return the the iid of the created incident, and it's uri so
 * that the client knows how to access it. A get-request to the uri
 * will return the full incident report.
 * 
 *  Format of $incident to create:
 * 		'uid' -> id of the user that submitted the incident : int(10)
 * 		'subject' -> Subject of the incident  : varchar(255)
 * 		'description' -> description of the incident : mediumtext
 * 		'created' -> timestamp of the creation date : bigint(20)
 * 		'date' -> timestamp of the incident date : bigint(20)
 * 		'location' -> location name : varchar(255)
 * 		'latitude' -> GPS Latitude coordinates : float
 * 		'longitude' -> GPS Longitude coordinates : float
 * 
 * 
 * @param object $data
 * @return object
 * 
 */
 function _nms_incidents_create($data) {
 	
	global $user;
	
	$data = (object) $data;
	
	unset($data->iid);
	$data->uid = $user->uid;
	$data->created = time();
	
	if(!isset($data->subject)) {
		return services_error('Missing incident atribute : subject', 406);
	}

	if(!isset($data->description)) {
		return services_error('Missing incident atribute : description', 406);
	}
	
	if(!isset($data->date)) {
		return services_error('Missing incident atribute : date', 406);
	}
	
	if(!isset($data->location)) {
		return services_error('Missing incident atribute : location', 406);
	}
	
	if(!isset($data->latitude)) {
		return services_error('Missing incident atribute : latitude', 406);
	}
	
	if(!isset($data->longitude)) {
		return services_error('Missing incident atribute : longitude', 406);
	}
	
	nms_incidents_write_incident($data);
	
	
	return (object) array(
    	'iid' => $data->iid,
    	'uri' => services_resource_uri(array('incidents', $data->iid)),
    	);
 }

/**
 * Callback for updating incident reports.
 * 
 * The update callback works more or less like the create callback, but 
 * we don't have to check that all the fields exists. There is no harm
 * in allowing a client to just update the subject and leave the rest alone.
 * 
 * @param int $iid
 * @param object $data
 * @return object
 */
function _nms_incidents_update($id, $ReceivedData) {
	
	global $user;
	
	$data = (object) $ReceivedData;
	
	unset($data->id);
	
	//$incident = nms_incidents_get_incident($id)->fetchObject();

	$data->iid = $id;
	$data->uid = $user->uid;
	
	nms_incidents_write_incident($data);
	
	return (object) array(
			'iid' => $id,
			'uri' => services_resource_uri(array('incidents', $data->iid)), 
			);
}

/**
 * Callback for retrieving incident reports.
 * 
 * @param int $iid
 * @return object
 */
 function _nms_incidents_retrieve($iid) {
 	return nms_incidents_get_incident($iid)->fetchObject();
 }


 /**
  * Callback for deleting incident reports.
  * 
  * @param int $iid
  * @return object
  */
  function _nms_incidents_delete($iid) {
  	nms_incidents_delete_incident($iid);
	
	return (object) array('iid' => $iid);
  }

/**
 * Callback for listing incidents
 * 
 * @param int $page
 * @param array $parameters
 * @return object
 */
 function _nms_incidents_index($page, $parameters) {
 	global $user;
 	
	$incidents = array();
	$query = nms_incidents_get_incidents_by_user($user->uid);
	
	foreach ($query as $incident) {
		$incidents[] = $incident;
	}
	
	return $incidents;
 }

 /**
  * Callback for listing all incidents on the system
  * 
  * @return object
  */
 function _nms_incidents_getAll() {
 		
 	$query = db_select('incidents', 'i'); //SELECT incidents table
 	$query->join('locations', 'l', 'i.lid = l.lid'); //JOIN incidents with locations
	$query->orderBy('i.created'); //ORDER BY uid
	$query->condition('i.iid', $id, '='); //Where iid = $id
	$query->fields('i') //SELECT * From incidents
	      ->fields('l', array('latitude', 'longitude')); //SELECT fields from locations
	$query->addField('l', 'name', 'location');
	
	$query->execute();
	
	$incidents = array();
	foreach ($query as $incident) {
		$incidents[] = $incident;
	}
	
	return (object) $incidents; 
	
 } 