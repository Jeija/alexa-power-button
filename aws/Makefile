FUNCTION_NAME	:= alexa-power-button
SOURCEFILES	:= index.js
LIBRARIES	:= validation.js

upload:
	@zip -r upload.zip $(SOURCEFILES) $(LIBRARIES)
	aws lambda update-function-code --function-name $(FUNCTION_NAME) --zip-file fileb://./upload.zip
	@$(RM) upload.zip

test_discover:
	@echo "LOG:"
	@echo "=========="
	@aws lambda invoke --function-name $(FUNCTION_NAME) --log-type Tail --payload file://./events/discover.json response.txt | awk -F $$'\t' 'BEGIN {OFS = FS} {print $$1}' | base64 --decode
	@echo -e "=========="

	@echo "RESPONSE:"
	@echo "=========="
	@cat response.txt
	@echo -e "\n=========="

	@$(RM) response.txt

test_turnon:
	@echo "LOG:"
	@echo "=========="
	@aws lambda invoke --function-name $(FUNCTION_NAME) --log-type Tail --payload file://./events/turnon.json response.txt | awk -F $$'\t' 'BEGIN {OFS = FS} {print $$1}' | base64 --decode
	@echo -e "=========="

	@echo "RESPONSE:"
	@echo "=========="
	@cat response.txt
	@echo -e "\n=========="

	@$(RM) response.txt

linter:
	jshint $(SOURCEFILES)
