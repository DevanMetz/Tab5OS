idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CHAT_HAS_SECRETS=1)
    set_property(SOURCE "main.c" APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/chat_secrets.h")
endif()
