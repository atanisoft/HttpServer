cmake_minimum_required(VERSION 3.10)

file(GLOB SOURCES src/*.cpp)
set(INCLUDE_PATH include)
set(DEPS lwip mbedtls OpenMRNIDF)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_PATH}
    REQUIRES ${DEPS})

if(CONFIG_HTTP_DNS_LIGHT_OPENMRN_LIB)
target_compile_options(${COMPONENT_LIB} PRIVATE -DESP32)
endif()

set_source_files_properties(src/HttpRequest.cpp PROPERTIES COMPILE_FLAGS -Wno-ignored-qualifiers)
set_source_files_properties(src/HttpRequestWebSocket.cpp PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough -Wno-ignored-qualifiers")
set_source_files_properties(src/HttpRequestFlow.cpp PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough -Wno-ignored-qualifiers")
set_source_files_properties(src/HttpResponse.cpp PROPERTIES COMPILE_FLAGS -Wno-ignored-qualifiers)
set_source_files_properties(src/HttpServer.cpp PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough -Wno-ignored-qualifiers")
