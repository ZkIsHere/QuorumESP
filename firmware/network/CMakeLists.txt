# Cert embedding for TLS dev (docs/tls.md §3). PEMs live in firmware/certs_dev/
# (git-ignored, never committed). They are baked into the binary as C string
# literals generated at configure time — no objcopy symbol guessing.
# Without certs the build still passes; TLS is refused at runtime (fail-closed).
set(TLS_SRCS "ethernet.c" "tls.c" "wifi.c" "time_sync.c")
set(TLS_DEFS "")
set(CERT_NAMES dev-ca.crt dev-server.crt dev-server.key)
set(CERT_VARS qesp_dev_ca_crt qesp_dev_server_crt qesp_dev_server_key)
set(HAVE_ALL_CERTS TRUE)
foreach(f ${CERT_NAMES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../certs_dev/${f}")
    set(HAVE_ALL_CERTS FALSE)
  endif()
endforeach()
if(HAVE_ALL_CERTS)
  set(GEN_C "${CMAKE_CURRENT_BINARY_DIR}/certs_embed.c")
  file(WRITE "${GEN_C}" "/* Generated: embedded dev TLS certs. DO NOT EDIT. */\n")
  list(LENGTH CERT_NAMES _n)
  math(EXPR _last "${_n} - 1")
  foreach(i RANGE 0 ${_last})
    list(GET CERT_NAMES ${i} _f)
    list(GET CERT_VARS ${i} _v)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../certs_dev/${_f}" _pem)
    string(REPLACE "\\" "\\\\" _pem "${_pem}")
    string(REPLACE "\"" "\\\"" _pem "${_pem}")
    string(REPLACE "\n" "\\n\"\n\"" _pem "${_pem}")
    file(APPEND "${GEN_C}" "const char ${_v}[] = \"${_pem}\";\n")
  endforeach()
  list(APPEND TLS_SRCS "${GEN_C}")
  list(APPEND TLS_DEFS "HAS_DEV_CERTS=1")
endif()

idf_component_register(SRCS ${TLS_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES log esp_wifi esp_netif esp_event esp-tls mbedtls esp_timer)
if(TLS_DEFS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ${TLS_DEFS})
endif()
