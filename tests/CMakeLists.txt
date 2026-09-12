add_executable(test_r5900 test_r5900.cpp)
target_link_libraries(test_r5900 PRIVATE ee::r5900)
add_test(NAME r5900 COMMAND test_r5900)

add_executable(test_elf test_elf.cpp)
target_link_libraries(test_elf PRIVATE ee::elf)
add_test(NAME elf COMMAND test_elf)

add_executable(test_analysis test_analysis.cpp)
target_include_directories(test_analysis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_analysis PRIVATE ee::analysis)
add_test(NAME analysis COMMAND test_analysis)

add_executable(test_codegen test_codegen.cpp)
target_include_directories(test_codegen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_codegen PRIVATE ee::codegen)
add_test(NAME codegen COMMAND test_codegen)

add_executable(test_runtime test_runtime.cpp)
target_link_libraries(test_runtime PRIVATE ee::runtime)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_kernel test_kernel.cpp)
target_link_libraries(test_kernel PRIVATE ee::runtime)
add_test(NAME kernel COMMAND test_kernel)

add_executable(test_hw test_hw.cpp)
target_link_libraries(test_hw PRIVATE ee::runtime)
add_test(NAME hw COMMAND test_hw)

add_executable(test_vu test_vu.cpp)
target_link_libraries(test_vu PRIVATE ee::runtime)
add_test(NAME vu COMMAND test_vu)

add_executable(test_cdvd test_cdvd.cpp)
target_link_libraries(test_cdvd PRIVATE ee::runtime)
add_test(NAME cdvd COMMAND test_cdvd)
