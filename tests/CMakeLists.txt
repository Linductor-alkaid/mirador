add_executable(mirador_core_smoke_test core/smoke_test.cpp)
target_link_libraries(mirador_core_smoke_test PRIVATE mirador::core)

add_test(NAME mirador.core.smoke COMMAND mirador_core_smoke_test)
set_tests_properties(mirador.core.smoke PROPERTIES LABELS "unit")
