cmake_minimum_required(VERSION 3.20)
project(opcua_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# (bleibt) open62541-Optionen und OpenSSL
set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
set(UA_ENABLE_SUBSCRIPTION_EVENTS ON CACHE BOOL "" FORCE)
#set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
if(MSVC)
  # /MD (Release) und /MDd (Debug)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(UA_ENABLE_ENCRYPTION OPENSSL CACHE STRING "" FORCE)
set(UA_ENABLE_OPENSSL    ON      CACHE BOOL   "" FORCE)
set(UA_ENABLE_MBEDTLS    OFF     CACHE BOOL   "" FORCE)
find_package(OpenSSL REQUIRED)

# (bleibt) open62541 als Subprojekt einbinden
add_subdirectory(open62541)   # bewährter Weg laut open62541/CMake Quick-Hinweisen :contentReference[oaicite:1]{index=1}

add_executable(opcua_client)

# Deine Quellen gesammelt ans Ziel hängen (moderner, zielbasierter Stil)
target_sources(opcua_client PRIVATE
  src/main.cpp
  src/PLCMonitor.cpp
  src/TaskForce.cpp
)

# Header-Verzeichnis für dieses Ziel
target_include_directories(opcua_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/open62541/include
  ${CMAKE_CURRENT_SOURCE_DIR}/open62541/plugins/include
  ${CMAKE_CURRENT_BINARY_DIR}/open62541/src_generated
)

# Linken gegen open62541 (+ transitiv OpenSSL über open62541)
target_link_libraries(opcua_client PRIVATE open62541)

# Optional: Zert/Key in den Build-Ordner kopieren (falls du DER nutzt und zur Laufzeit von dort lädst)
#add_custom_command(TARGET opcua_client POST_BUILD
#  COMMAND ${CMAKE_COMMAND} -E copy_if_different
#          ${CMAKE_SOURCE_DIR}/certs/client_cert.der
#          ${CMAKE_SOURCE_DIR}/certs/client_key.der
#          $<TARGET_FILE_DIR:opcua_client>
#)
