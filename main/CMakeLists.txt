set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(SRCS "main.cpp" "SttClient.cpp" "VoicePipeline.cpp" "VoiceWebServer.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES
                         nvsstoragemanager
                         wifimanager
                         settingsbase
                         mqttwrapper
                         webserver
                         jsonwrapper
                         espressif__mqtt
                         board
                         esp_http_client
                         esp_wifi
                         esp_netif
                         esp_event
                         nvs_flash
                         app_update
                         esp_timer)
# esp-sr is pulled in via idf_component.yml and auto-added to this component's
# requirements (its managed-component name is not spelled here to avoid a
# mismatch).
