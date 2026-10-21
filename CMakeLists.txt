cmake_minimum_required(VERSION 3.22)

project(VoiChanAlgoBox VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Visual C++ のランタイムを EXE と VST3 の中に組み込む（利用者の PC に Visual C++ 再頒布可能パッケージが
# なくても動くように）。DeepFilterNet の Rust のラッパーも同じ形（crt-static）でビルドする
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ソースは UTF-8（日本語の文字列を含むため）
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_subdirectory(external/JUCE)

# ---- WORLD（分析合成方式のライブラリ。ソースを取り込んでそのままビルドする） ----
add_library(World STATIC
    external/World/src/cheaptrick.cpp
    external/World/src/codec.cpp
    external/World/src/common.cpp
    external/World/src/d4c.cpp
    external/World/src/dio.cpp
    external/World/src/fft.cpp
    external/World/src/harvest.cpp
    external/World/src/matlabfunctions.cpp
    external/World/src/stonemask.cpp
    external/World/src/synthesis.cpp
    external/World/src/synthesisrealtime.cpp
)
target_include_directories(World PUBLIC external/World/src)
set_target_properties(World PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(World PRIVATE /W0)
endif()

# ---- RNNoise（AI のデノイズ。Xiph、BSD ライセンス） ----
# 学習済みの重み（ソースの形で約 78MB）はリポジトリに含まれないので、公式の配布元から取得し、
# SHA-256 を照合してからビルドの作業フォルダに展開する（初回の設定のときだけ。インターネットが必要）
file(READ external/rnnoise/model_version RNNOISE_MODEL_HASH)
string(STRIP "${RNNOISE_MODEL_HASH}" RNNOISE_MODEL_HASH)
set(RNNOISE_MODEL_DIR ${CMAKE_BINARY_DIR}/rnnoise_model)
if(NOT EXISTS ${RNNOISE_MODEL_DIR}/src/rnnoise_data.c)
    message(STATUS "RNNoise の学習済みの重みを取得します")
    file(DOWNLOAD https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz
         ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz
         EXPECTED_HASH SHA256=${RNNOISE_MODEL_HASH}
         STATUS RNNOISE_DOWNLOAD_STATUS)
    list(GET RNNOISE_DOWNLOAD_STATUS 0 RNNOISE_DOWNLOAD_CODE)
    if(NOT RNNOISE_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "RNNoise の重みを取得できませんでした: ${RNNOISE_DOWNLOAD_STATUS}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz DESTINATION ${RNNOISE_MODEL_DIR}
         PATTERNS src/rnnoise_data.c src/rnnoise_data.h)
    file(REMOVE ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz)
endif()
add_library(RNNoise STATIC
    external/rnnoise/src/denoise.c
    external/rnnoise/src/rnn.c
    external/rnnoise/src/pitch.c
    external/rnnoise/src/kiss_fft.c
    external/rnnoise/src/celt_lpc.c
    external/rnnoise/src/nnet.c
    external/rnnoise/src/nnet_default.c
    external/rnnoise/src/parse_lpcnet_weights.c
    external/rnnoise/src/rnnoise_tables.c
    ${RNNOISE_MODEL_DIR}/src/rnnoise_data.c
)
target_include_directories(RNNoise PUBLIC external/rnnoise/include PRIVATE ${RNNOISE_MODEL_DIR}/src external/rnnoise/src)
target_compile_definitions(RNNoise PUBLIC RNNOISE_EXPORT=)
set_target_properties(RNNoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(RNNoise PRIVATE /W0)
endif()

# ---- WebRTC のノイズ抑制（external/webrtc_ns。必要なファイルだけを抜き出したもの。経緯は external/webrtc_ns/README.md） ----
add_library(WebRtcNs STATIC
    external/webrtc_ns/webrtc/modules/audio_processing/ns/fast_math.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/histograms.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/noise_estimator.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/noise_suppressor.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/ns_fft.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/prior_signal_model.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/prior_signal_model_estimator.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/quantile_noise_estimator.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/signal_model.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/signal_model_estimator.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/speech_probability_estimator.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/suppression_params.cc
    external/webrtc_ns/webrtc/modules/audio_processing/ns/wiener_filter.cc
    external/webrtc_ns/webrtc/modules/audio_processing/three_band_filter_bank.cc
    external/webrtc_ns/webrtc/common_audio/third_party/ooura/fft_size_256/fft4g.cc
)
target_include_directories(WebRtcNs PUBLIC external/webrtc_ns/webrtc)
target_compile_features(WebRtcNs PUBLIC cxx_std_17)
set_target_properties(WebRtcNs PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(WebRtcNs PRIVATE /w)
    target_compile_definitions(WebRtcNs PUBLIC _USE_MATH_DEFINES NOMINMAX)
endif()

# ---- 処理の部品（JUCE に依存しない） ----
add_library(VoiChanAlgoBoxCore STATIC
    Source/Core/Choices.cpp
    Source/Core/Params.cpp
    Source/Core/ParamRelevance.cpp
    Source/Core/Engine.cpp
    Source/Core/LatencyMeter.cpp
    Source/Core/Dsp/Gate.cpp
    Source/Core/Dsp/Limiter.cpp
    Source/Core/Dsp/PitchDetector.cpp
    Source/Core/Dsp/Notch.cpp
    Source/Core/Dsp/Denoise.cpp
    Source/Core/Dsp/NoiseTracker.cpp
    Source/Core/Dsp/BandDenoise.cpp
    Source/Core/Dsp/SpectralDenoise.cpp
    Source/Core/Dsp/AiDenoise.cpp
    Source/Core/Dsp/WebRtcNs.cpp
    Source/Core/Dsp/Transient.cpp
    Source/Core/Dsp/Repair.cpp
    Source/Core/Dsp/ClickRepair.cpp
    Source/Core/Dsp/FormantTracker.cpp
    Source/Core/Dsp/PitchSwipe.cpp
    Source/Core/Dsp/PitchRapt.cpp
    Source/Core/Dsp/PitchSift.cpp
    Source/Core/Dsp/PitchPyin.cpp
    Source/Core/Dsp/PitchWorld.cpp
    Source/Core/Dsp/HarvestStream.cpp
    Source/Core/Algorithms/Algorithms.cpp
    Source/Core/Algorithms/DelayLineShifter.cpp
    Source/Core/Algorithms/WsolaShifter.cpp
    Source/Core/Algorithms/PsolaShifter.cpp
    Source/Core/Algorithms/PhaseVocoder.cpp
    Source/Core/Algorithms/SinusoidalModel.cpp
    Source/Core/Algorithms/LpcVoice.cpp
    Source/Core/Algorithms/ChannelVocoder.cpp
    Source/Core/Algorithms/FrequencyShifter.cpp
    Source/Core/Algorithms/BandShifter.cpp
    Source/Core/Algorithms/WorldVoice.cpp
    Source/Core/Algorithms/SourceExciter.cpp
    Source/Core/Dsp/SpectralEnvelope.cpp
)
target_include_directories(VoiChanAlgoBoxCore PUBLIC Source/Core)
target_link_libraries(VoiChanAlgoBoxCore PRIVATE World RNNoise WebRtcNs)

# ---- DeepFilterNet（AI のデノイズ）。Rust のラッパー（Source/DeepFilter）を静的ライブラリにして組み込む ----
# Rust（cargo）が見つからないときは組み込まない（その場合、方式の選択欄の DeepFilterNet は素通しになる）。
# 初回の設定のときに、依存するライブラリを取得してビルドする（数分かかる。インターネットが必要）
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
option(VCAB_DEEPFILTER "DeepFilterNet を組み込む（Rust が必要）" ON)
if(VCAB_DEEPFILTER AND CARGO_EXECUTABLE)
    set(DFN_TARGET_DIR ${CMAKE_BINARY_DIR}/dfn)
    set(DFN_LIB ${DFN_TARGET_DIR}/release/vcab_dfn.lib)
    # 低遅延のモデル（DeepFilterNet3_ll、約 36MB）は、DeepFilterNet のリポジトリの同じコミットから取得し、SHA-256 を照合する
    set(DFN_LL_MODEL ${CMAKE_BINARY_DIR}/dfn_model/DeepFilterNet3_ll_onnx.tar.gz)
    set(DFN_LL_SHA256 5998e58e8ba0e09bb76986ef97b84afa065a571ef282d4a1222f341e3251cf3a)
    if(NOT EXISTS ${DFN_LL_MODEL})
        message(STATUS "DeepFilterNet の低遅延のモデルを取得します")
        file(DOWNLOAD https://github.com/Rikorose/DeepFilterNet/raw/d375b2d8309e0935d165700c91da9de862a99c31/models/DeepFilterNet3_ll_onnx.tar.gz
             ${DFN_LL_MODEL} EXPECTED_HASH SHA256=${DFN_LL_SHA256} STATUS DFN_LL_STATUS)
        list(GET DFN_LL_STATUS 0 DFN_LL_CODE)
        if(NOT DFN_LL_CODE EQUAL 0)
            file(REMOVE ${DFN_LL_MODEL})
            message(FATAL_ERROR "DeepFilterNet の低遅延のモデルを取得できませんでした: ${DFN_LL_STATUS}")
        endif()
    endif()
    set(DFN_BUILD_COMMAND ${CMAKE_COMMAND} -E env VCAB_DFN_LL_MODEL=${DFN_LL_MODEL}
        CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-Ctarget-feature=+crt-static
        ${CARGO_EXECUTABLE} build --release --locked --features ll-model --target-dir ${DFN_TARGET_DIR})
    message(STATUS "DeepFilterNet のラッパーをビルドします（初回は数分かかります）")
    execute_process(COMMAND ${DFN_BUILD_COMMAND}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/Source/DeepFilter
                    RESULT_VARIABLE DFN_RESULT)
    if(NOT DFN_RESULT EQUAL 0)
        message(FATAL_ERROR "DeepFilterNet のラッパーをビルドできませんでした。Rust がないときは -DVCAB_DEEPFILTER=OFF で組み込まずにビルドできます")
    endif()
    # ラッパーを書き換えたときに作り直す（変わっていなければ、すぐ終わる）
    add_custom_target(vcab_dfn_build
                      COMMAND ${DFN_BUILD_COMMAND}
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/Source/DeepFilter
                      BYPRODUCTS ${DFN_LIB})
    # Rust の標準ライブラリが使う Windows のライブラリ（cargo rustc -- --print native-static-libs で調べたもの）。
    # windows.0.52.0.lib は、cargo が取得した windows_x86_64_msvc の中にある
    get_filename_component(CARGO_HOME_DIR ${CARGO_EXECUTABLE} DIRECTORY)
    get_filename_component(CARGO_HOME_DIR ${CARGO_HOME_DIR} DIRECTORY)
    file(GLOB DFN_WINDOWS_LIB_DIRS LIST_DIRECTORIES true "${CARGO_HOME_DIR}/registry/src/*/windows_x86_64_msvc-0.52.*/lib")
    add_library(DeepFilter STATIC IMPORTED)
    set_target_properties(DeepFilter PROPERTIES
        IMPORTED_LOCATION ${DFN_LIB}
        INTERFACE_LINK_DIRECTORIES "${DFN_WINDOWS_LIB_DIRS}"
        INTERFACE_LINK_LIBRARIES "bcrypt;advapi32;windows.0.52.0;kernel32;ntdll;userenv;ws2_32;dbghelp")
    add_dependencies(VoiChanAlgoBoxCore vcab_dfn_build)
    target_link_libraries(VoiChanAlgoBoxCore PRIVATE DeepFilter)
    target_compile_definitions(VoiChanAlgoBoxCore PRIVATE VCAB_HAS_DEEPFILTER=1)
else()
    message(STATUS "Rust（cargo）が見つからないので、DeepFilterNet は組み込みません")
endif()
set_target_properties(VoiChanAlgoBoxCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(VoiChanAlgoBoxCore PRIVATE /W4)
endif()

# 使わない形式（音声の FLAC・Ogg Vorbis・Opus、画像の WebP）を JUCE から組み込まない（WAV と PNG だけ使う。
# 組み込むと、そのライブラリのライセンス文も配布物に添える必要がある）
set(VCAB_UNUSED_FORMATS JUCE_USE_FLAC=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_OPUS=0 JUCE_USE_WEBP=0)

# ---- プラグイン（VST3）と単体アプリ（EXE） ----
juce_add_plugin(VoiChanAlgoBox
    COMPANY_NAME "VoiChanAlgoBox"
    PRODUCT_NAME "VoiChanAlgoBox"
    PLUGIN_MANUFACTURER_CODE Vcha
    PLUGIN_CODE Vcab
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx
    ICON_BIG Resources/Icon.png          # アイコン（Tools/Icon/make_icon.py で作る）
    ICON_SMALL Resources/IconSmall.png   # 小さい表示用（線を太くしたもの）
)

target_sources(VoiChanAlgoBox PRIVATE
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/Plugin/EditorTabs.cpp
    Source/Plugin/LookAndFeel.cpp
    Source/Plugin/MonitorView.cpp
    Source/Plugin/MonitorViewEq.cpp
    Source/Plugin/DynamicsView.cpp
    Source/Plugin/PitchView.cpp
    Source/Standalone/VCStandaloneApp.cpp
)

target_compile_definitions(VoiChanAlgoBox PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_ASIO=1
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1  # EXE 版の外枠は Source/Standalone の自作のもの
    ${VCAB_UNUSED_FORMATS}
)

target_link_libraries(VoiChanAlgoBox
    PRIVATE
        VoiChanAlgoBoxCore
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---- 開発用の WAV 処理ツール ----
juce_add_console_app(vcab_render PRODUCT_NAME "vcab_render")

target_sources(vcab_render PRIVATE Tools/Render/Main.cpp)

target_compile_definitions(vcab_render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    ${VCAB_UNUSED_FORMATS}
)

target_link_libraries(vcab_render
    PRIVATE
        VoiChanAlgoBoxCore
        juce::juce_audio_formats
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
