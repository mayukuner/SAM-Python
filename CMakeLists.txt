cmake_minimum_required(VERSION 2.8.12)
project(sam)

SET(SOURCE_DIR "src/sam")
# Tell cmake that headers are in alse in source_dir
include_directories(${SOURCE_DIR})
# SET(SOURCES "${SOURCE_DIR}/amalgamation.cpp")

SET(SOURCES "${SOURCE_DIR}/utils.cpp"
            "${SOURCE_DIR}/math.cpp"
            "${SOURCE_DIR}/c_api.cpp")
aux_source_directory("${SOURCE_DIR}/backend/c_api" SOURCES)
aux_source_directory("${SOURCE_DIR}/backend/solver" SOURCES)
aux_source_directory("${SOURCE_DIR}/backend/objective" SOURCES)

MESSAGE( STATUS "variable is: " ${SOURCES} )


SET(TEST_DIR "tests")
SET(TESTS ${SOURCES}
    "${TEST_DIR}/test_main.cpp"
    "${TEST_DIR}/test_math.cpp")

# Generate a test executable
# include_directories(lib/catch/include)
# add_executable("${PROJECT_NAME}_test" ${TESTS})

# Generate python module
add_subdirectory(lib/pybind11)
pybind11_add_module(sam "${SOURCE_DIR}/bindings.cpp" ${SOURCES})
