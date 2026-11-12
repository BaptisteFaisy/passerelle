# SPDX-License-Identifier: Apache-2.0
#
# tests/sbom - the SBOM and the build provenance are generated and validated.
#
# Gap 39 of docs/gap-analysis.md: the tree had no bill of materials and no
# record of how it was built. tools/sbom/generate_sbom.py walks the real
# inputs (the C sources, the VERSION file, the toolchain, the git commit) and
# writes a CycloneDX 1.5 document plus a provenance file; this test runs it
# and fails when the document is not valid.
#
# The unit level (what makes a document invalid) is covered by
# tools/sbom/test_sbom.py, run by pytest in release.yml.

add_test(NAME sbom-valid
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/sbom/generate_sbom.py"
            --source-dir "${CMAKE_SOURCE_DIR}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/passerelle.sbom.json"
            --provenance "${CMAKE_CURRENT_BINARY_DIR}/provenance.json"
            --check)
set_tests_properties(sbom-valid PROPERTIES
    TIMEOUT 120
    LABELS "sbom")
