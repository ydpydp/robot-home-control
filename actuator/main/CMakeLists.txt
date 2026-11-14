idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_driver_gpio esp_system)

# Device identity: the broadcast name doubles as the central's
# selector. Build one binary per lamp:
#   idf.py -DRHC_DEVICE_NAME=RHC-Livingroom build
#   idf.py -DRHC_DEVICE_NAME=RHC-Bedroom build
# Default (no -D) is the living-room lamp.
if(NOT DEFINED RHC_DEVICE_NAME)
    set(RHC_DEVICE_NAME "RHC-Livingroom")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RHC_DEVICE_NAME="${RHC_DEVICE_NAME}")
