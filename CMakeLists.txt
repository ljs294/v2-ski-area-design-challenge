cmake_minimum_required(VERSION 3.24)
project(SkiDomainTests LANGUAGES CXX)

file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" compiler_path)
file(WRITE "${CMAKE_BINARY_DIR}/toolchain.json"
    "{\n"
    "  \"cmake\": \"${CMAKE_VERSION}\",\n"
    "  \"generator\": \"${CMAKE_GENERATOR}\",\n"
    "  \"compiler\": \"${compiler_path}\",\n"
    "  \"compiler_id\": \"${CMAKE_CXX_COMPILER_ID}\",\n"
    "  \"compiler_version\": \"${CMAKE_CXX_COMPILER_VERSION}\",\n"
    "  \"toolset\": \"${CMAKE_VS_PLATFORM_TOOLSET}\",\n"
    "  \"windows_sdk\": \"${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\"\n"
    "}\n")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Build/domain-sources.json" manifest)
string(JSON source_count LENGTH "${manifest}" sources)
math(EXPR last_source "${source_count} - 1")
set(domain_sources)
foreach(index RANGE ${last_source})
    string(JSON source GET "${manifest}" sources ${index})
    list(APPEND domain_sources "${source}")
endforeach()
add_library(SkiDomain STATIC ${domain_sources})
target_compile_definitions(SkiDomain PUBLIC SKI_DOMAIN_STANDALONE=1)
target_include_directories(SkiDomain PUBLIC Source/SkiDomain/Public)
if(MSVC)
    target_compile_options(SkiDomain PRIVATE /W4 /WX)
else()
    target_compile_options(SkiDomain PRIVATE -Wall -Wextra -Werror)
endif()

# Require the missing-header diagnostic, not merely a failed compile mentioning it.
function(ski_missing_header diagnostic header result)
    if(MSVC)
        set(pattern "C1083: Cannot open include file: '${header}':")
    else()
        set(pattern "${header}: No such file or directory|'${header}' file not found")
    endif()
    if("${diagnostic}" MATCHES "${pattern}")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Actual compiler rejection, including a transitive header; no Unreal include paths.
foreach(fixture IN ITEMS ForbiddenDirect ForbiddenTransitive)
    try_compile(accepted "${CMAKE_BINARY_DIR}/negative/${fixture}"
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/Domain/${fixture}.cpp"
        OUTPUT_VARIABLE diagnostic)
    file(WRITE "${CMAKE_BINARY_DIR}/negative/${fixture}.log" "${diagnostic}")
    if(fixture STREQUAL "ForbiddenDirect")
        set(header "CoreMinimal[.]h")
    else()
        set(header "UObject[/\\\\]Object[.]h")
    endif()
    ski_missing_header("${diagnostic}" "${header}" missing)
    if(accepted OR NOT missing)
        message(FATAL_ERROR "${fixture} did not demonstrate the expected forbidden-header rejection: ${diagnostic}")
    endif()
endforeach()

# Prove that a found header which fails parsing cannot satisfy that gate.
try_compile(resolved_accepted "${CMAKE_BINARY_DIR}/negative/ForbiddenResolved"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/Domain/ForbiddenDirect.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_CURRENT_SOURCE_DIR}/tests/Domain/ResolvedUnrealStub"
    OUTPUT_VARIABLE resolved_diagnostic)
file(WRITE "${CMAKE_BINARY_DIR}/negative/ForbiddenResolved.log" "${resolved_diagnostic}")
ski_missing_header("${resolved_diagnostic}" "CoreMinimal[.]h" resolved_missing)
if(resolved_accepted OR resolved_missing OR NOT resolved_diagnostic MATCHES "SKI_P0_RESOLVED_HEADER")
    message(FATAL_ERROR "Resolved-header negative control did not reject the false pass: ${resolved_diagnostic}")
endif()

add_executable(SkiDomainTests tests/Domain/RevisionTests.cpp)
target_link_libraries(SkiDomainTests PRIVATE SkiDomain)
add_executable(SkiTerrainDomainTests tests/Domain/TerrainTests.cpp)
target_link_libraries(SkiTerrainDomainTests PRIVATE SkiDomain)
add_executable(SkiTerrainCoreDomainTests tests/Domain/TerrainCoreTests.cpp)
target_link_libraries(SkiTerrainCoreDomainTests PRIVATE SkiDomain)
add_executable(SkiCoverEcologyDomainTests tests/Domain/CoverEcologyTests.cpp)
target_link_libraries(SkiCoverEcologyDomainTests PRIVATE SkiDomain)
add_executable(SkiSiteSelectionDomainTests tests/Domain/SiteSelectionTests.cpp)
target_link_libraries(SkiSiteSelectionDomainTests PRIVATE SkiDomain)
add_executable(SkiTerrainQualityDomainTests tests/Domain/TerrainQualityTests.cpp)
target_link_libraries(SkiTerrainQualityDomainTests PRIVATE SkiDomain)
add_executable(SkiElevationSourcesDomainTests tests/Domain/ElevationSourcesTests.cpp)
target_link_libraries(SkiElevationSourcesDomainTests PRIVATE SkiDomain)
add_executable(SkiPlaceCoordinatesDomainTests tests/Domain/PlaceCoordinatesTests.cpp)
target_link_libraries(SkiPlaceCoordinatesDomainTests PRIVATE SkiDomain)
enable_testing()
add_test(NAME SkiDomain.Revision COMMAND SkiDomainTests)
set_tests_properties(SkiDomain.Revision PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.Terrain COMMAND SkiTerrainDomainTests)
set_tests_properties(SkiDomain.Terrain PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.TerrainCore COMMAND SkiTerrainCoreDomainTests)
set_tests_properties(SkiDomain.TerrainCore PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.CoverEcology COMMAND SkiCoverEcologyDomainTests)
set_tests_properties(SkiDomain.CoverEcology PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.SiteSelection COMMAND SkiSiteSelectionDomainTests)
set_tests_properties(SkiDomain.SiteSelection PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.TerrainQuality COMMAND SkiTerrainQualityDomainTests)
set_tests_properties(SkiDomain.TerrainQuality PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.ElevationSources COMMAND SkiElevationSourcesDomainTests)
set_tests_properties(SkiDomain.ElevationSources PROPERTIES TIMEOUT 10)
add_test(NAME SkiDomain.PlaceCoordinates COMMAND SkiPlaceCoordinatesDomainTests)
set_tests_properties(SkiDomain.PlaceCoordinates PROPERTIES TIMEOUT 10)
