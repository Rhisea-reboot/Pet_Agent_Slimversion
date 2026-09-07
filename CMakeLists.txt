cmake_minimum_required(VERSION 3.16)

project(VPet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Sql Concurrent)

include(CTest)

set(VPET_TOOL_SOURCES
    src/agent/tools/itool.cpp
    src/agent/tools/tool_registry.cpp
    src/agent/tools/tool_scheduler.cpp
    src/agent/tools/tool_loop_executor.cpp
    src/agent/tool_loop_node.cpp
    include/vpet/agent/tools/itool.h
    include/vpet/agent/tools/tool_registry.h
    include/vpet/agent/tools/tool_scheduler.h
    include/vpet/agent/tools/tool_loop_executor.h
    include/vpet/agent/tool_loop_node.h
)
# Apply after all existing targets have been declared.
cmake_language(DEFER CALL vpet_add_tool_targets)
function(vpet_add_tool_targets)
    foreach(target VPet dag_hot_reload_tests dag_http_server_tests agent_runtime_scheduler_tests application_integration_tests memory_dag_integration_tests)
        if(TARGET ${target})
            target_sources(${target} PRIVATE ${VPET_TOOL_SOURCES}
                src/agent/tools/native_tools.cpp include/vpet/agent/tools/native_tools.h
                src/sensor/screenshot_sensor.cpp include/vpet/sensor/screenshot_sensor.h)
            target_link_libraries(${target} PRIVATE Qt6::Gui)
        endif()
    endforeach()
    if(BUILD_TESTING)
        foreach(target tool_scheduler_tests llm_tool_calling_tests tool_loop_tests)
            add_executable(${target} tests/${target}.cpp ${VPET_TOOL_SOURCES}
                src/llm/llm_client.cpp include/vpet/llm/llm_client.h
                src/agent/agent_context.cpp)
            target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
            target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Network Qt6::Test)
            set_property(TARGET ${target} PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)
            add_test(NAME ${target} COMMAND ${target})
        endforeach()
    endif()
endfunction()

# 官方 onnxruntime C API 头文件位于 include/onnxruntime 子目录
set(VPET_VENDOR_ONNXRUNTIME_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/onnxruntime/include/onnxruntime
)

set(VPET_VENDOR_ONNXRUNTIME_DLL
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/onnxruntime/bin/onnxruntime.dll
)

# 将 qsqlite 插件部署到目标目录（Qt 默认从 <appdir>/plugins 搜索插件）
function(vpet_deploy_qt_sqlite TARGET_NAME)
    if(TARGET Qt6::QSqliteDriverPlugin)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/plugins/sqldrivers"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QSqliteDriverPlugin>"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/plugins/sqldrivers/"
        )
    endif()
endfunction()

# 将 onnxruntime.dll 复制到目标目录（仅当 vendor 目录存在时）
function(vpet_deploy_onnxruntime TARGET_NAME)
    if(EXISTS ${VPET_VENDOR_ONNXRUNTIME_DLL})
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VPET_VENDOR_ONNXRUNTIME_DLL}
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/"
        )
    endif()
endfunction()

set(VPET_SOURCES
    src/main.cpp
    src/main_window.cpp
    src/common_types.cpp
    src/animation_frame.cpp
    src/animation_segment.cpp
    src/animation_clip.cpp
    src/animation_resource_manager.cpp
    src/pet_state_machine.cpp
    src/pet_controller.cpp
    src/pet_config.cpp
    src/bubble_message.cpp
    src/hit_region.cpp
    src/say_dialog.cpp
    src/tts_client.cpp
    src/tts_audio_player.cpp
    src/stream_sentence_splitter.cpp
    src/stream_dialogue_coordinator.cpp
    src/tts_server_manager.cpp
    src/chat_bubble_window.cpp
    src/splash_window.cpp
    src/perception/frame_buffer.cpp
    src/perception/perception_pipeline.cpp
    src/perception/vision_encoder.cpp
    src/sensor/screenshot_sensor.cpp
    src/speech/voice_input_manager.cpp
    src/llm/llm_client.cpp
    src/llm/vision_llm_client.cpp
    src/web/web_search_client.cpp
    src/web/web_search_tool.cpp
    src/web/web_research_engine.cpp
    src/agent/agent_context.cpp
    src/agent/agent_dag_graph.cpp
    src/agent/emotion_rewrite_node.cpp
    src/agent/proactive_topic_node.cpp
    src/agent/web_research_node.cpp
    src/agent/invocation_queue_policy.cpp
    src/agent/agent_output_policy.cpp
    src/agent/agent_node_registry.cpp
    src/agent/agent_graph_executor.cpp
    src/agent/agent_async_bridge.cpp
    src/agent/agent_runtime.cpp
    src/agent/agent_runtime_scheduler.cpp
    src/agent/agent_runtime_nodes.cpp
    src/agent/agent_runtime_async.cpp
    src/dageditor/agent_node_catalog.cpp
    src/dageditor/dag_document.cpp
    src/dageditor/agent_dag_validator.cpp
    src/dageditor/dag_document_store.cpp
    src/dageditor/dag_http_server.cpp
    src/dageditor/dag_editor_server.cpp
    src/agent/memory_retrieve_node.cpp
    src/agent/memory_store_node.cpp
    src/memory/memory_graph.cpp
    src/memory/memory_repository.cpp
    src/memory/memory_service.cpp
    src/memory/memory_maintenance.cpp
    src/memory/memory_consolidator.cpp
    src/memory/embedding_client.cpp
    src/memory/vector_store.cpp
    src/memory/memory_manager_dialog.cpp
)

set(VPET_HEADERS
    include/vpet/common_types.h
    include/vpet/animation_frame.h
    include/vpet/animation_segment.h
    include/vpet/animation_clip.h
    include/vpet/animation_resource_manager.h
    include/vpet/pet_state_machine.h
    include/vpet/pet_controller.h
    include/vpet/pet_config.h
    include/vpet/bubble_message.h
    include/vpet/hit_region.h
    include/vpet/say_dialog.h
    include/vpet/tts_client.h
    include/vpet/tts_audio_player.h
    include/vpet/stream_sentence_splitter.h
    include/vpet/stream_dialogue_coordinator.h
    include/vpet/tts_server_manager.h
    include/vpet/chat_bubble_window.h
    include/vpet/splash_window.h
    include/vpet/perception/frame_buffer.h
    include/vpet/perception/perception_pipeline.h
    include/vpet/perception/vision_encoder.h
    include/vpet/sensor/screenshot_sensor.h
    include/vpet/speech/voice_input_manager.h
    include/vpet/llm/llm_client.h
    include/vpet/llm/vision_llm_client.h
    include/vpet/web/web_search_client.h
    include/vpet/web/web_search_tool.h
    include/vpet/web/web_research_engine.h
    include/vpet/agent/agent_context.h
    include/vpet/agent/agent_context_keys.h
    include/vpet/agent/agent_dag_graph.h
    include/vpet/agent/emotion_rewrite_node.h
    include/vpet/agent/proactive_topic_node.h
    include/vpet/agent/web_research_node.h
    include/vpet/agent/invocation_queue_policy.h
    include/vpet/agent/agent_output_policy.h
    include/vpet/agent/agent_node_registry.h
    include/vpet/agent/agent_graph_executor.h
    include/vpet/agent/agent_async_bridge.h
    include/vpet/agent/agent_runtime.h
    include/vpet/dageditor/agent_node_catalog.h
    include/vpet/dageditor/dag_document.h
    include/vpet/dageditor/agent_dag_validator.h
    include/vpet/dageditor/dag_document_store.h
    include/vpet/dageditor/dag_http_request.h
    include/vpet/dageditor/dag_editor_server.h
    include/vpet/agent/memory_retrieve_node.h
    include/vpet/agent/memory_store_node.h
    include/vpet/memory/memory_graph.h
    include/vpet/memory/memory_repository.h
    include/vpet/memory/memory_service.h
    include/vpet/memory/memory_maintenance.h
    include/vpet/memory/memory_consolidator.h
    include/vpet/memory/embedding_client.h
    include/vpet/memory/vector_store.h
    include/vpet/memory/memory_manager_dialog.h
    src/main_window.h
)

add_executable(${PROJECT_NAME} ${VPET_SOURCES} ${VPET_HEADERS}
    resources/dag_editor/dag_editor_web.qrc)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Multimedia
        Qt6::Sql
        Qt6::Concurrent
)

vpet_deploy_qt_sqlite(${PROJECT_NAME})
vpet_deploy_onnxruntime(${PROJECT_NAME})

if(WIN32)
    set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE TRUE)
endif()

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    add_executable(agent_dag_graph_tests
        tests/agent_dag_graph_test.cpp
        src/agent/agent_dag_graph.cpp
        src/agent/agent_context.cpp
        include/vpet/agent/agent_dag_graph.h
        include/vpet/agent/agent_context.h
    )

    target_include_directories(agent_dag_graph_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(agent_dag_graph_tests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    set_property(TARGET agent_dag_graph_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME agent_dag_graph_tests COMMAND agent_dag_graph_tests)

    add_executable(agent_node_catalog_tests
        tests/agent_node_catalog_test.cpp
        src/dageditor/agent_node_catalog.cpp
        include/vpet/dageditor/agent_node_catalog.h
    )

    target_include_directories(agent_node_catalog_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_definitions(agent_node_catalog_tests PRIVATE
        DAG_EDITOR_EXAMPLE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/agent_dag_structure.example.json"
    )

    target_link_libraries(agent_node_catalog_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Test
    )

    set_property(TARGET agent_node_catalog_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME agent_node_catalog_tests COMMAND agent_node_catalog_tests)

    add_executable(dag_document_tests
        tests/dag_document_test.cpp
        src/dageditor/dag_document.cpp
        include/vpet/dageditor/dag_document.h
    )

    target_include_directories(dag_document_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(dag_document_tests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    set_property(TARGET dag_document_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME dag_document_tests COMMAND dag_document_tests)

    add_executable(dag_validator_tests
        tests/dag_validator_test.cpp
        src/dageditor/agent_node_catalog.cpp
        src/dageditor/dag_document.cpp
        src/dageditor/agent_dag_validator.cpp
        include/vpet/dageditor/agent_node_catalog.h
        include/vpet/dageditor/dag_document.h
        include/vpet/dageditor/agent_dag_validator.h
    )

    target_include_directories(dag_validator_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_definitions(dag_validator_tests PRIVATE
        DAG_EDITOR_EXAMPLE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/agent_dag_structure.example.json"
    )

    target_link_libraries(dag_validator_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Test
    )

    set_property(TARGET dag_validator_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME dag_validator_tests COMMAND dag_validator_tests)

    add_executable(dag_hot_reload_tests
        tests/dag_hot_reload_test.cpp
        src/agent/agent_context.cpp
        src/agent/agent_dag_graph.cpp
        src/dageditor/agent_node_catalog.cpp
        src/dageditor/dag_document.cpp
        src/dageditor/agent_dag_validator.cpp
        src/agent/emotion_rewrite_node.cpp
        src/agent/proactive_topic_node.cpp
        src/agent/web_research_node.cpp
        src/agent/invocation_queue_policy.cpp
        src/agent/agent_output_policy.cpp
        src/agent/agent_node_registry.cpp
        src/agent/agent_graph_executor.cpp
        src/agent/agent_async_bridge.cpp
        src/agent/agent_runtime.cpp
        src/agent/agent_runtime_scheduler.cpp
        src/agent/agent_runtime_nodes.cpp
        src/agent/agent_runtime_async.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        src/agent/memory_retrieve_node.cpp
        src/agent/memory_store_node.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        src/llm/llm_client.cpp
        src/llm/vision_llm_client.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
        include/vpet/agent/agent_context.h
        include/vpet/agent/agent_context_keys.h
        include/vpet/agent/agent_dag_graph.h
        include/vpet/agent/emotion_rewrite_node.h
        include/vpet/agent/proactive_topic_node.h
        include/vpet/agent/web_research_node.h
        include/vpet/agent/invocation_queue_policy.h
        include/vpet/agent/agent_output_policy.h
        include/vpet/agent/agent_node_registry.h
        include/vpet/agent/agent_graph_executor.h
        include/vpet/agent/agent_async_bridge.h
        include/vpet/agent/agent_runtime.h
        include/vpet/dageditor/agent_node_catalog.h
        include/vpet/dageditor/dag_document.h
        include/vpet/dageditor/agent_dag_validator.h
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
        include/vpet/agent/memory_retrieve_node.h
        include/vpet/agent/memory_store_node.h
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
        include/vpet/llm/llm_client.h
        include/vpet/llm/vision_llm_client.h
    )

    target_include_directories(dag_hot_reload_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(dag_hot_reload_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(dag_hot_reload_tests)
    vpet_deploy_onnxruntime(dag_hot_reload_tests)

    set_property(TARGET dag_hot_reload_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME dag_hot_reload_tests COMMAND dag_hot_reload_tests)

    add_executable(dag_document_store_tests
        tests/dag_document_store_test.cpp
        src/dageditor/agent_node_catalog.cpp
        src/dageditor/dag_document.cpp
        src/dageditor/agent_dag_validator.cpp
        src/dageditor/dag_document_store.cpp
        include/vpet/dageditor/agent_node_catalog.h
        include/vpet/dageditor/dag_document.h
        include/vpet/dageditor/agent_dag_validator.h
        include/vpet/dageditor/dag_document_store.h
    )

    target_include_directories(dag_document_store_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(dag_document_store_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Test
    )

    set_property(TARGET dag_document_store_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME dag_document_store_tests COMMAND dag_document_store_tests)

    add_executable(dag_http_server_tests
        tests/dag_http_server_test.cpp
        resources/dag_editor/dag_editor_web.qrc
        src/agent/agent_context.cpp
        src/agent/agent_dag_graph.cpp
        src/dageditor/agent_node_catalog.cpp
        src/dageditor/dag_document.cpp
        src/dageditor/agent_dag_validator.cpp
        src/dageditor/dag_document_store.cpp
        src/dageditor/dag_http_server.cpp
        src/dageditor/dag_editor_server.cpp
        src/agent/emotion_rewrite_node.cpp
        src/agent/proactive_topic_node.cpp
        src/agent/web_research_node.cpp
        src/agent/invocation_queue_policy.cpp
        src/agent/agent_output_policy.cpp
        src/agent/agent_node_registry.cpp
        src/agent/agent_graph_executor.cpp
        src/agent/agent_async_bridge.cpp
        src/agent/agent_runtime.cpp
        src/agent/agent_runtime_scheduler.cpp
        src/agent/agent_runtime_nodes.cpp
        src/agent/agent_runtime_async.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        src/agent/memory_retrieve_node.cpp
        src/agent/memory_store_node.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        src/llm/llm_client.cpp
        src/llm/vision_llm_client.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
        include/vpet/agent/agent_context.h
        include/vpet/agent/agent_context_keys.h
        include/vpet/agent/agent_runtime.h
        include/vpet/agent/agent_dag_graph.h
        include/vpet/dageditor/agent_node_catalog.h
        include/vpet/dageditor/dag_document.h
        include/vpet/dageditor/agent_dag_validator.h
        include/vpet/dageditor/dag_document_store.h
        include/vpet/dageditor/dag_http_request.h
        include/vpet/dageditor/dag_editor_server.h
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
        include/vpet/llm/llm_client.h
        include/vpet/llm/vision_llm_client.h
    )

    target_include_directories(dag_http_server_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(dag_http_server_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(dag_http_server_tests)
    vpet_deploy_onnxruntime(dag_http_server_tests)

    set_property(TARGET dag_http_server_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME dag_http_server_tests COMMAND dag_http_server_tests)

    add_executable(stream_sentence_splitter_tests
        tests/stream_sentence_splitter_test.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
    )

    target_include_directories(stream_sentence_splitter_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(stream_sentence_splitter_tests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    set_property(TARGET stream_sentence_splitter_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME stream_sentence_splitter_tests COMMAND stream_sentence_splitter_tests)

    add_executable(pet_config_tests
        tests/pet_config_test.cpp
        src/pet_config.cpp
        include/vpet/pet_config.h
    )

    target_include_directories(pet_config_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(pet_config_tests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    set_property(TARGET pet_config_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME pet_config_tests COMMAND pet_config_tests)

    add_executable(agent_runtime_scheduler_tests
        tests/agent_runtime_scheduler_test.cpp
        src/agent/agent_context.cpp
        src/agent/agent_dag_graph.cpp
        src/agent/emotion_rewrite_node.cpp
        src/agent/proactive_topic_node.cpp
        src/agent/web_research_node.cpp
        src/agent/invocation_queue_policy.cpp
        src/agent/agent_output_policy.cpp
        src/agent/agent_node_registry.cpp
        src/agent/agent_graph_executor.cpp
        src/agent/agent_async_bridge.cpp
        src/agent/agent_runtime.cpp
        src/agent/agent_runtime_scheduler.cpp
        src/agent/agent_runtime_nodes.cpp
        src/agent/agent_runtime_async.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        src/agent/memory_retrieve_node.cpp
        src/agent/memory_store_node.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        src/llm/llm_client.cpp
        src/llm/vision_llm_client.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
        include/vpet/agent/agent_context.h
        include/vpet/agent/agent_context_keys.h
        include/vpet/agent/agent_dag_graph.h
        include/vpet/agent/emotion_rewrite_node.h
        include/vpet/agent/proactive_topic_node.h
        include/vpet/agent/web_research_node.h
        include/vpet/agent/invocation_queue_policy.h
        include/vpet/agent/agent_output_policy.h
        include/vpet/agent/agent_node_registry.h
        include/vpet/agent/agent_graph_executor.h
        include/vpet/agent/agent_async_bridge.h
        include/vpet/agent/agent_runtime.h
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
        include/vpet/agent/memory_retrieve_node.h
        include/vpet/agent/memory_store_node.h
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
        include/vpet/llm/llm_client.h
        include/vpet/llm/vision_llm_client.h
    )

    target_include_directories(agent_runtime_scheduler_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(agent_runtime_scheduler_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(agent_runtime_scheduler_tests)
    vpet_deploy_onnxruntime(agent_runtime_scheduler_tests)

    set_property(TARGET agent_runtime_scheduler_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME agent_runtime_scheduler_tests COMMAND agent_runtime_scheduler_tests)

    add_executable(application_integration_tests
        tests/application_integration_test.cpp
        src/agent/agent_context.cpp
        src/agent/agent_dag_graph.cpp
        src/agent/emotion_rewrite_node.cpp
        src/agent/proactive_topic_node.cpp
        src/agent/web_research_node.cpp
        src/agent/invocation_queue_policy.cpp
        src/agent/agent_output_policy.cpp
        src/agent/agent_node_registry.cpp
        src/agent/agent_graph_executor.cpp
        src/agent/agent_async_bridge.cpp
        src/agent/agent_runtime.cpp
        src/agent/agent_runtime_scheduler.cpp
        src/agent/agent_runtime_nodes.cpp
        src/agent/agent_runtime_async.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        src/agent/memory_retrieve_node.cpp
        src/agent/memory_store_node.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        src/llm/llm_client.cpp
        src/llm/vision_llm_client.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
        include/vpet/agent/agent_context.h
        include/vpet/agent/agent_context_keys.h
        include/vpet/agent/agent_dag_graph.h
        include/vpet/agent/emotion_rewrite_node.h
        include/vpet/agent/proactive_topic_node.h
        include/vpet/agent/web_research_node.h
        include/vpet/agent/invocation_queue_policy.h
        include/vpet/agent/agent_output_policy.h
        include/vpet/agent/agent_node_registry.h
        include/vpet/agent/agent_graph_executor.h
        include/vpet/agent/agent_async_bridge.h
        include/vpet/agent/agent_runtime.h
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
        include/vpet/agent/memory_retrieve_node.h
        include/vpet/agent/memory_store_node.h
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
        include/vpet/llm/llm_client.h
        include/vpet/llm/vision_llm_client.h
    )

    target_include_directories(application_integration_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(application_integration_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(application_integration_tests)
    vpet_deploy_onnxruntime(application_integration_tests)

    set_property(TARGET application_integration_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME application_integration_tests COMMAND application_integration_tests)

    add_executable(web_search_client_tests
        tests/web_search_client_test.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
    )

    target_include_directories(web_search_client_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(web_search_client_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Test
    )

    set_property(TARGET web_search_client_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME web_search_client_tests COMMAND web_search_client_tests)

    add_executable(web_research_engine_tests
        tests/web_research_engine_test.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
    )

    target_include_directories(web_research_engine_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(web_research_engine_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Test
    )

    set_property(TARGET web_research_engine_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME web_research_engine_tests COMMAND web_research_engine_tests)

    add_executable(web_search_daemon_integration_tests
        tests/web_search_daemon_integration_test.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
    )

    target_include_directories(web_search_daemon_integration_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(web_search_daemon_integration_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Test
    )

    set_property(TARGET web_search_daemon_integration_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME web_search_daemon_integration_tests COMMAND web_search_daemon_integration_tests)

    add_executable(memory_core_tests
        tests/memory_core_test.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_repository.cpp
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_repository.h
    )

    target_include_directories(memory_core_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(memory_core_tests
        PRIVATE
            Qt6::Core
            Qt6::Test
    )

    set_property(TARGET memory_core_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME memory_core_tests COMMAND memory_core_tests)

    add_executable(memory_service_tests
        tests/memory_service_test.cpp
        src/llm/llm_client.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/llm/llm_client.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
    )

    target_include_directories(memory_service_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(memory_service_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(memory_service_tests)
    vpet_deploy_onnxruntime(memory_service_tests)

    set_property(TARGET memory_service_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME memory_service_tests COMMAND memory_service_tests)

    add_executable(memory_dag_integration_tests
        tests/memory_dag_integration_test.cpp
        src/agent/agent_context.cpp
        src/agent/agent_dag_graph.cpp
        src/agent/emotion_rewrite_node.cpp
        src/agent/proactive_topic_node.cpp
        src/agent/web_research_node.cpp
        src/agent/invocation_queue_policy.cpp
        src/agent/agent_output_policy.cpp
        src/agent/agent_node_registry.cpp
        src/agent/agent_graph_executor.cpp
        src/agent/agent_async_bridge.cpp
        src/agent/agent_runtime.cpp
        src/agent/agent_runtime_scheduler.cpp
        src/agent/agent_runtime_nodes.cpp
        src/agent/agent_runtime_async.cpp
        src/agent/memory_retrieve_node.cpp
        src/agent/memory_store_node.cpp
        src/memory/memory_graph.cpp
        src/memory/memory_repository.cpp
        src/memory/memory_service.cpp
        src/memory/memory_maintenance.cpp
        src/memory/memory_consolidator.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        src/web/web_search_client.cpp
        src/web/web_search_tool.cpp
        src/web/web_research_engine.cpp
        src/llm/llm_client.cpp
        src/llm/vision_llm_client.cpp
        src/stream_sentence_splitter.cpp
        include/vpet/stream_sentence_splitter.h
        include/vpet/agent/agent_context.h
        include/vpet/agent/agent_context_keys.h
        include/vpet/agent/agent_dag_graph.h
        include/vpet/agent/emotion_rewrite_node.h
        include/vpet/agent/proactive_topic_node.h
        include/vpet/agent/web_research_node.h
        include/vpet/agent/invocation_queue_policy.h
        include/vpet/agent/agent_output_policy.h
        include/vpet/agent/agent_node_registry.h
        include/vpet/agent/agent_graph_executor.h
        include/vpet/agent/agent_async_bridge.h
        include/vpet/agent/agent_runtime.h
        include/vpet/agent/memory_retrieve_node.h
        include/vpet/agent/memory_store_node.h
        include/vpet/memory/memory_graph.h
        include/vpet/memory/memory_repository.h
        include/vpet/memory/memory_service.h
        include/vpet/memory/memory_maintenance.h
        include/vpet/memory/memory_consolidator.h
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
        include/vpet/web/web_search_client.h
        include/vpet/web/web_search_tool.h
        include/vpet/web/web_research_engine.h
        include/vpet/llm/llm_client.h
        include/vpet/llm/vision_llm_client.h
    )

    target_include_directories(memory_dag_integration_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(memory_dag_integration_tests
        PRIVATE
            Qt6::Core
            Qt6::Network
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(memory_dag_integration_tests)
    vpet_deploy_onnxruntime(memory_dag_integration_tests)

    set_property(TARGET memory_dag_integration_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME memory_dag_integration_tests COMMAND memory_dag_integration_tests)

    add_executable(memory_embedding_tests
        tests/memory_embedding_test.cpp
        src/memory/embedding_client.cpp
        src/memory/vector_store.cpp
        include/vpet/memory/embedding_client.h
        include/vpet/memory/vector_store.h
    )

    target_include_directories(memory_embedding_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${VPET_VENDOR_ONNXRUNTIME_INCLUDE}
    )

    target_link_libraries(memory_embedding_tests
        PRIVATE
            Qt6::Core
            Qt6::Sql
            Qt6::Test
    )

    vpet_deploy_qt_sqlite(memory_embedding_tests)
    vpet_deploy_onnxruntime(memory_embedding_tests)

    set_property(TARGET memory_embedding_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME memory_embedding_tests COMMAND memory_embedding_tests)

    add_executable(perception_pipeline_tests
        tests/perception_pipeline_test.cpp
        src/perception/frame_buffer.cpp
        src/perception/perception_pipeline.cpp
        src/perception/vision_encoder.cpp
        src/sensor/screenshot_sensor.cpp
        include/vpet/perception/frame_buffer.h
        include/vpet/perception/perception_pipeline.h
        include/vpet/perception/vision_encoder.h
        include/vpet/sensor/screenshot_sensor.h
    )

    target_include_directories(perception_pipeline_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(perception_pipeline_tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Concurrent
            Qt6::Test
    )

    set_property(TARGET perception_pipeline_tests PROPERTY AUTOMOC_COMPILER_PREDEFINES OFF)

    add_test(NAME perception_pipeline_tests COMMAND perception_pipeline_tests)
endif()
