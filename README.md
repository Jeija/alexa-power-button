# Alexa / Dash Button = PC power button
If you like to hide your desktop computer case so that it's not in the way, you probably know that it can be quite annoying to have to press the power button in an uncomfortable location every time you want to turn on your PC. `alexa-power-button` solves this by enabling you to use both voice commands for Amazon's digital assistant "Alexa" and/or an Amazon Dash Button to start your computer.

## Hardware
`alexa-power-button` doesn't use Wake on LAN so that you can save energy by turning off all of the attached network equipment apart from a single WiFi router in your home. Instead, the only additional active device is a small ESP32-based PCB inside the computer case that can be programmed and powered using the mainboard's internal USB connectors. Most mainboards can charge phones and other USB devices while the PC is turned off, and this current is more than enough to power the ESP32. The PCB contains n-channel MOSFETs that can be used to pull the power / reset button data line low. This assumes that the power / reset button input of the mainboard consists of an active-low input pin with a positive bias voltage. This pin should then be connected to the MOSFET's drain.

The KiCad design files (schematic, PCB layout) are provided in `hardware`.

## Dash Button Setup
* Use the Amazon App to configure the dash button
* In the last step, don't select which of the products in the list to order
* Press the dash button and use wireshark or similar tools to get the button's MAC address
* In the `firmware` directory: Execute `make menuconfig` and enter the Dash Button's MAC Address in `Alexa Power Button --> Dash Button MAC Address` (including colons)

After flashing the firmware, the ESP32 will capture all WiFi traffic and check if source or destination address of the IEEE 802.11 header match the provided Dash Button MAC address.

## Alexa Skill + AWS MQTT / Lambda Setup
* Follow some of the [commonly available](https://www.hackster.io/darian-johnson/diy-smart-lamp-controlled-by-toggle-switch-and-alexa-7de243) [tutorials](https://developer.amazon.com/blogs/post/Tx4WG410EHXIYQ/five-steps-before-developing-a-smart-home-skill) on how to create an Alexa Skill, set up a Lambda function and set up AWS IoT for MQTT
* Make sure to select Payload Version `v2 (legacy)` when creating the Alexa Skill
* Upload the Lambda function. If you have the AWS CLI set up, you can use the Makefile provided in `aws` to upload and test the code.
* Add `THING_ENDPOINT` (AWS IoT HTTPS REST API endpoint host, see the thing's Interact page in AWS IoT) and `MQTT_BASE_TOPIC` (prefix for MQTT topic, e.g. `myhome/mypc`) as environment variables for the Lambda function
* Get your device private key and device certificate from AWS IoT and copy them to `firmware/main/certs` as `private.pem.key` and `certificate.pem.crt` respectively. See [esp-idf's AWS IoT documentation](https://github.com/espressif/esp-idf/tree/master/examples/protocols/aws_iot) for details.
* Confgiure the firmware: Run `make menuconfig` in `firmware` and add your WiFi SSID and password in `Alexa Power Button`. Configure your (arbitrary but unique) AWS IoT Client ID and add the same MQTT base topic as specified in your Lambda function environment variable. Under `Component Config --> Amazon Web Services IoT Platform`, add your AWS IoT HTTPS REST API endpoint host, see the thing's Interact page on AWS IoT.
* Flash the firmware. If errors occur while connecting to AWS IoT, see [the documentation in esp-idf](https://github.com/espressif/esp-idf/tree/master/examples/protocols/aws_iot) for some common problems.

## Flash the firmware
* Use `make menuconfig` in `firmware` to configure your python executable, serial port and flasher baud rate
* `alexa-power-button` uses the usual esp-idf Makefile and workflow. Run `make flash` in `firmware` in order to compile and flash the firmware to your ESP32 board.

## Attribution
* ESP32 schematic symbol and footprint: [adamjvr's ESP32-kiCAD-Footprints
](https://github.com/adamjvr/ESP32-kiCAD-Footprints)
* Alexa Skill Lambda function validation (`aws/validation.js`): [Amazon's alexa-smarthome-validation](https://github.com/alexa/alexa-smarthome-validation)
* ESP32 code: Roughly based on esp-idf's [AWS IoT Subscribe/Publish Example](https://github.com/espressif/esp-idf/tree/master/examples/protocols/aws_iot/subscribe_publish)
