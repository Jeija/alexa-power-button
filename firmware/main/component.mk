#
# Main Makefile. This is basically the same as a component makefile.
# Based on https://github.com/espressif/esp-idf/tree/master/examples/protocols/aws_iot/subscribe_publish
#

# Certificate files. certificate.pem.crt & private.pem.key must be downloaded
# from AWS, see README for details.
COMPONENT_EMBED_TXTFILES := certs/aws-root-ca.pem certs/certificate.pem.crt certs/private.pem.key

# Print an error if the certificate/key files are missing
$(COMPONENT_PATH)/certs/certificate.pem.crt $(COMPONENT_PATH)/certs/private.pem.key:
	@echo "Missing PEM file $@. This file identifies the ESP32 to AWS, see README for details."
	exit 1
