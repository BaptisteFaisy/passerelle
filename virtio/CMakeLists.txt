# SPDX-License-Identifier: Apache-2.0
#
# virtio/CMakeLists.txt - layer 1, the virtio transport.
#
# Builds one static library, pgw_virtio, out of portable C11 only: the mock
# device, the split virtqueue, the MMIO transport and the modern PCI transport
# all run on the PC. The QNX 8.0 bindings (libpci, mmap_device_io,
# InterruptAttachThread) live in *_qnx.c and are compiled only for a QNX target,
# so the PC build never needs the SDP.
#
# The public contract is include/pgw/virtio.h; the internal layout shared by the
# backends is src/virtio_priv.h, which the tests include to inspect the rings.

set(PGW_VIRTIO_SRCS
    src/dev.c
    src/mock.c
    src/mmio.c
    src/pci.c
    src/pci_caps.c
    src/pci_modern.c
    src/virtq.c
    src/virtq_alloc.c
    src/virtq_dev.c
)

if(QNX)
    list(APPEND PGW_VIRTIO_SRCS
        src/qnx_mem.c
        src/mmio_qnx.c
        src/pci_qnx.c
    )
endif()

pgw_library(pgw_virtio
    SRCS ${PGW_VIRTIO_SRCS}
    DEPS pgw_common
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(BUILD_TESTING)
    # One test binary per area, each linked against the library only, so a
    # failure names the area instead of one giant suite.
    foreach(area dev vq mock mmio pci)
        pgw_test(test_virtio_${area}
            SRCS tests/test_${area}.c
            DEPS pgw_virtio
        )
        target_include_directories(test_virtio_${area} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    endforeach()
endif()

# The QNX files can also be syntax-checked on the PC against tests/qnx-stub,
# when that stub tree exists (written by another agent). Off by default, and
# skipped silently while the stubs are missing.
if(PGW_QNX_STUBS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/qnx-stub/include")
    pgw_qnx_syntax(pgw_virtio_qnx_syntax
        SRCS src/qnx_mem.c src/mmio_qnx.c src/pci_qnx.c
        DEPS pgw_common
    )
    target_include_directories(pgw_virtio_qnx_syntax PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
