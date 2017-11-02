const AWS = require("aws-sdk");
const validator = require("./validation");

/*
 * aws-sdk-js is used for AWS IoT API Access:
 *
 * This requires the following environment variables to be set:
 * `AWS_DEFAULT_REGION` (e.g. "eu-west-1"), `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`
 * These environment variables are automatically provided by the lambda function execution role.
 * Just make sure that the lambda function's execution role permits AWS IoT access.
 *
 * Provide the thing endpoint as a `THING_ENDPOINT` environment variable.
 * Provide the MQTT base topic as `MQTT_BASE_TOPIC` environment variable (same as in ESP32 firmware).
 */
var iot_thing = new AWS.IotData({ endpoint : process.env.THING_ENDPOINT });

function handleDiscovery(request, context) {
	var payload = {
		"discoveredAppliances":
		[
			{
				"applianceId": "alexa-power-button",
				"manufacturerName": "DIY",
				"modelName" : "DIY PCB",
				"version" : "1",
				"isReachable" : true,
				"friendlyName": "Alexa Power Button",
				"friendlyDescription": "ESP32 PC Power Button with AWS IoT MQTT",
				"actions": [
			        	"turnOn",
					"turnOff"
				],
				"additionalApplianceDetails": {
					"extraDetail1": "Power your PC on or off with an Alexa command"
				}
			}
		]
	};

	var responseHeader = {
		"payloadVersion": "2",
		"namespace": "Alexa.ConnectedHome.Discovery",
		"name": "DiscoverAppliancesResponse",
		"messageId": request.header.messageId + "-R"
	};

	var response = { header: responseHeader, payload: payload };

	/*
	 * Validate response
	 */
	try {
		validator.validateResponse(request, response);
	} catch (error) {
		console.log("Discovery response validation failed: " + error);
	}

	context.succeed(response);
}

function handlePowerControl(request, context) {
	var responseName = "";
	var mqttMessage = "";

	/*
	 * Parse request
	 */
	if (request.header.name === "TurnOnRequest") {
		responseName = "TurnOnConfirmation";
		mqttMessage = "1";
	} else if (request.header.name === "TurnOffRequest") {
		responseName = "TurnOffConfirmation";
		mqttMessage = "0";
	}

	/*
	 * Prespare response
	 */
	var responseHeader = {
		"payloadVersion": "2",
		"namespace": "Alexa.ConnectedHome.Control",
		"name": responseName,
		"messageId": request.header.messageId + "-R"
	};

	var response = {
		header: responseHeader,
		payload: {}
	};

	/*
	 * Validate response
	 */
	try {
		validator.validateResponse(request, response);
	} catch (error) {
		console.log("Control response validation failed: " + error);
	}

	/*
	 * Send MQTT message and send response as soon as MQTT message is delivered
	 */
	iot_thing.publish({
		topic : process.env.MQTT_BASE_TOPIC + "/power",
		payload : mqttMessage,
		qos : 1
	}, function(err, data) {
		if (err) console.log(err, err.stack);
		else console.log(data);
		context.succeed(response);
	});
}

exports.handler = function(request, context) {
	/*
	 * Validate request
	 */
	try {
		validator.validateContext(context);
	} catch (error) {
		console.log("Request validation failed:" + error);
	}

	/*
	 * Parse command
	 */
	if (request.header.namespace === "Alexa.ConnectedHome.Discovery" && request.header.name === "DiscoverAppliancesRequest") {
		handleDiscovery(request, context, "");
	} else if (request.header.namespace === "Alexa.ConnectedHome.Control") {
		if (request.header.name === "TurnOnRequest" || request.header.name === "TurnOffRequest") {
			handlePowerControl(request, context);
		}
	}
};
