
set(CHATLIB_RTCM_HEADERS
    rtcModule/IVideoRenderer.h
    rtcModule/rtcmPrivate.h
    rtcModule/rtcStats.h
    rtcModule/webrtcAdapter.h
    rtcModule/webrtc.h
    rtcModule/webrtcPrivate.h
)

set(CHATLIB_RTCM_SOURCES
    rtcModule/rtcStats.cpp
    rtcModule/webrtcAdapter.cpp
    rtcModule/webrtc.cpp
)


target_sources(CHATlib
    PRIVATE
    rtcModule/IRtcCrypto.h
)

target_sources_conditional(CHATlib
    FLAG USE_WEBRTC
    PRIVATE
    ${CHATLIB_RTCM_HEADERS}
    ${CHATLIB_RTCM_SOURCES}
)

target_include_directories(CHATlib PUBLIC ${CMAKE_CURRENT_LIST_DIR})
