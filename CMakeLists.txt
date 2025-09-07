cmake_minimum_required(VERSION 3.20)
project(opcua_client CXX)

# C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC Runtime (wie zuvor)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# open62541 Optionen (wie zuvor)
set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
set(UA_ENABLE_SUBSCRIPTION_EVENTS ON CACHE BOOL "" FORCE)
set(UA_ENABLE_ENCRYPTION OPENSSL CACHE STRING "" FORCE)
set(UA_ENABLE_OPENSSL    ON      CACHE BOOL   "" FORCE)
set(UA_ENABLE_MBEDTLS    OFF     CACHE BOOL   "" FORCE)
find_package(OpenSSL REQUIRED)

# Subprojekte (wie zuvor)
add_subdirectory(open62541)
add_subdirectory(extern/pybind11)

# nlohmann/json (wie zuvor)
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(extern/nlohmann_json)

# Executable (Client)
add_executable(opcua_client
  src/main.cpp
  src/PLCMonitor.cpp
  src/EventBus.cpp
  src/ReactionManager.cpp   # ← falls umbenannt
  # src/TaskManager.cpp     # ← alternativ, falls alter Name
)

target_sources(opcua_client PRIVATE
  include/Event.h
  include/ReactiveObserver.h
  include/EventBus.h
  include/ReactionManager.h   # oder TaskManager.h
)

# Includes (eigene + open62541 generated)
target_include_directories(opcua_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/open62541/include
  ${CMAKE_CURRENT_SOURCE_DIR}/open62541/plugins/include
  ${CMAKE_CURRENT_BINARY_DIR}/open62541/src_generated
)

# Link (wie zuvor)
target_link_libraries(opcua_client PRIVATE
  open62541
  pybind11::embed
  nlohmann_json::nlohmann_json
)

# Python3 Embed (wie zuvor)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
target_link_libraries(opcua_client PRIVATE Python3::Python)

# Client-Zertifikate neben die EXE spiegeln (wie zuvor)
add_custom_command(TARGET opcua_client POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:opcua_client>/certificates"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/certificates/client_cert.der"
          "$<TARGET_FILE_DIR:opcua_client>/certificates/client_cert.der"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/certificates/client_key.der"
          "$<TARGET_FILE_DIR:opcua_client>/certificates/client_key.der"
)
