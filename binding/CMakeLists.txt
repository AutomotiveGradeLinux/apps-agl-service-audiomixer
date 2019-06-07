PROJECT_TARGET_ADD(audiomixer-binding)

	add_definitions(-DAFB_BINDING_VERSION=3)
	add_definitions(-DBUILDING_APPFW_BINDING)

	set(audiomixer_SOURCES
		audiomixer-binding.c
		audiomixer.c
	)

	add_library(${TARGET_NAME} MODULE ${audiomixer_SOURCES})

	SET_TARGET_PROPERTIES(${TARGET_NAME} PROPERTIES
		PREFIX "libafm-"
		LABELS "BINDING"
		LINK_FLAGS ${BINDINGS_LINK_FLAG}
		OUTPUT_NAME ${TARGET_NAME}
	)

	TARGET_LINK_LIBRARIES(${TARGET_NAME} ${link_libraries})
