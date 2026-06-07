#arduino-cli compile -v --fqbn esp32:esp32:esp32
arduino-cli compile -v --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs \
	--build-property "compiler.optimization_flags=-Os" \
	.
