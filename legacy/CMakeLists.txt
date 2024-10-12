set(SRC
  pkt.c
)

set(HDRS
  pkt.h
)

add_library(legacy ${SRC} ${HDRS})

target_link_libraries(legacy PUBLIC ${CJSON_LIBRARIES})

target_include_directories(legacy PRIVATE
  ${mosquitto_SOURCE_DIR} 
  ${mosquitto_SOURCE_DIR}/lib
  ${mosquitto_SOURCE_DIR}/include 
)
