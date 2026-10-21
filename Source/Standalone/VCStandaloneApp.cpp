// EXE 版の起動部分（JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP で、JUCE の標準のものと差し替えている）
//
// JUCE 9.0.4 の juce_audio_plugin_client_Standalone.cpp の StandaloneFilterApp を元にしている。
// 変更点:
//  - 自作の外枠（VCStandaloneWindow.h）を使う
//  - 起動時に、翻訳の表を設定して、オーディオの設定画面などを日本語にする（EXE 版だけ。VST3 を読み込んだ DAW には影響しない）
#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone

#include <juce_audio_utils/juce_audio_utils.h>

#include "VCStandaloneWindow.h"
#include "../Plugin/PluginEditor.h" // VCLookAndFeel（外枠と設定画面にも同じ見た目を使う）

namespace
{
// JUCE の画面の文字の翻訳（JUCE の TRANS で囲まれた文字が対象）
const char* const kJapaneseTranslations = R"TRANSLATIONS(language: Japanese
countries: jp

"Audio/MIDI Settings" = "オーディオ設定"
"Audio/MIDI Settings..." = "オーディオ設定..."
"Save current state..." = "今の設定をファイルに保存..."
"Load a saved state..." = "保存した設定を読み込む..."
"Reset to default state" = "すべて初期状態に戻す"
"Save current state" = "今の設定を保存"
"Load a saved state" = "保存した設定を読み込む"
"Error whilst saving" = "保存できませんでした"
"Couldn't write to the specified file!" = "指定したファイルに書き込めませんでした。"
"Error whilst loading" = "読み込めませんでした"
"Couldn't read from the specified file!" = "指定したファイルを読み込めませんでした。"

"Audio device type:" = "デバイスの種類:"
"Device:" = "デバイス:"
"Output:" = "出力:"
"Input:" = "入力:"
"Test" = "テスト"
"Plays a test tone" = "テスト音を鳴らします"
"Active output channels:" = "使う出力チャンネル:"
"Active input channels:" = "使う入力チャンネル:"
"(no audio output channels found)" = "（出力チャンネルが見つかりません）"
"(no audio input channels found)" = "（入力チャンネルが見つかりません）"
"Sample rate:" = "サンプルレート:"
"Audio buffer size:" = "バッファの大きさ:"
"Control Panel" = "コントロールパネル"
"Opens the device's own control panel" = "デバイスの設定画面を開きます"
"Reset Device" = "デバイスをリセット"
"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel" = "オーディオインターフェースをリセットします。デバイスの設定画面で設定を変えた後に必要になることがあります。"
"Show advanced settings..." = "詳細な設定を表示..."
"Error when trying to open audio device!" = "オーディオデバイスを開けませんでした。"
"none" = "なし"
"OK" = "OK"
)TRANSLATIONS";
} // namespace

namespace juce
{

class VCStandaloneApp final : public JUCEApplication
{
public:
    VCStandaloneApp()
    {
        PropertiesFile::Options options;
        options.applicationName     = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName          = "";
        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override              { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override           { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override              { return true; }
    void anotherInstanceStarted (const String&) override    {}

    void initialise (const String&) override
    {
        // 画面を作る前に、翻訳の表と見た目（文字の大きさの下限など）を設定する
        LocalisedStrings::setCurrentMappings (new LocalisedStrings (String (CharPointer_UTF8 (kJapaneseTranslations)), false));
        lookAndFeel = std::make_unique<VCLookAndFeel>();
        LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        if (Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            // 画面がないときは、ウィンドウを作らずに処理だけ動かす
            pluginHolder = createPluginHolder();
            return;
        }

        mainWindow = std::make_unique<VCStandaloneFilterWindow> (getApplicationName(),
                                                                 LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                                                 createPluginHolder());
        mainWindow->setVisible (true);
    }

    void shutdown() override
    {
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
        LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
        LocalisedStrings::setCurrentMappings (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

private:
    std::unique_ptr<VCStandalonePluginHolder> createPluginHolder()
    {
        auto holder = std::make_unique<VCStandalonePluginHolder> (appProperties.getUserSettings(),
                                                                  false,
                                                                  String{},
                                                                  nullptr,
                                                                  Array<VCStandalonePluginHolder::PluginInOuts>{},
                                                                  false); // MIDI デバイスは開かない
        // プラグインの画面の「オーディオ設定」ボタンから、オプションと同じ設定画面を開けるようにする
        if (auto* p = dynamic_cast<VoiChanAlgoBoxProcessor*> (holder->processor.get()))
        {
            auto* raw = holder.get();
            p->openAudioSettings = [raw] { raw->showAudioSettingsDialog(); };
        }
        return holder;
    }

    ApplicationProperties appProperties;
    std::unique_ptr<VCLookAndFeel> lookAndFeel;
    std::unique_ptr<VCStandaloneFilterWindow> mainWindow;
    std::unique_ptr<VCStandalonePluginHolder> pluginHolder;
};

} // namespace juce

// JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP のときに、JUCE から呼ばれる
juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new juce::VCStandaloneApp(); }

#endif
