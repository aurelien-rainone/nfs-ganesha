add_definitions(
  -D_FILE_OFFSET_BITS=64
)

SET(fsals3_LIB_SRCS
  up.c
  main.c
  export.c
  handle.c
  internal.c
  internal.h
  s3_methods.h
  s3_methods.c
)

message("LIBS3_INCLUDE_DIRS ${LIBS3_INCLUDE_DIRS}")
include_directories(${LIBS3_INCLUDE_DIRS})

add_library(fsals3 MODULE ${fsals3_LIB_SRCS})
add_sanitizers(fsals3)

target_link_libraries(fsals3 ${LIBS3_LIBRARIES} ${SYSTEM_LIBRARIES})

set_target_properties(fsals3 PROPERTIES VERSION 4.2.0 SOVERSION 4)
install(TARGETS fsals3 COMPONENT fsal DESTINATION ${FSAL_DESTINATION} )
