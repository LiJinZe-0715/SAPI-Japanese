#pragma once

// User-facing text for the installer and the uninstaller, in the machine's UI language.
//
// Three languages, picked by GetUserDefaultUILanguage: Japanese, English, Chinese. Anything else
// gets English. The command line tools deliberately stay English — they are developer tools, and
// their output is meant to be pasted into a bug report.

#include <windows.h>

#include <string>

namespace vvmsg {

enum class Lang { Japanese, English, Chinese };

inline Lang System() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE:
            return Lang::Japanese;
        case LANG_CHINESE:
            return Lang::Chinese;
        default:
            return Lang::English;
    }
}

inline const wchar_t* Pick(Lang lang, const wchar_t* ja, const wchar_t* en, const wchar_t* zh) {
    switch (lang) {
        case Lang::Japanese:
            return ja;
        case Lang::Chinese:
            return zh;
        default:
            return en;
    }
}

inline const wchar_t* Title() {
    return L"VOICEVOX \u97f3\u58f0 (SAPI5)";
}

inline std::wstring AdminRequired(Lang lang) {
    return Pick(lang, L"\u7ba1\u7406\u8005\u6a29\u9650\u304c\u5fc5\u8981\u3067\u3059\u3002",
                L"Administrator rights are required.", L"\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650\u3002");
}

inline std::wstring PayloadMissing(Lang lang) {
    return Pick(lang,
                L"\u3053\u306e\u30d5\u30a1\u30a4\u30eb\u306f\u5b8c\u5168\u306a\u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u3067\u306f\u3042\u308a\u307e\u305b\u3093\u3002\n"
                L"\uff08\u30da\u30a4\u30ed\u30fc\u30c9\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093\uff09",
                L"This file is not a complete installer: the payload is missing.\n\n"
                L"Download the packaged VoicevoxSapiSetup.exe instead of building it yourself.",
                L"\u8fd9\u4e2a\u6587\u4ef6\u4e0d\u662f\u5b8c\u6574\u7684\u5b89\u88c5\u5305\uff08\u7f3a\u5c11 payload\uff09\u3002\n\n"
                L"\u8bf7\u4f7f\u7528\u6253\u5305\u597d\u7684 VoicevoxSapiSetup.exe\u3002");
}

inline std::wstring WriteFailed(Lang lang, const std::wstring& path) {
    return std::wstring(
               Pick(lang, L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002\n\u66f8\u304d\u8fbc\u3081\u307e\u305b\u3093: ",
                    L"Installation failed. Could not write: ", L"\u5b89\u88c5\u5931\u8d25\uff0c\u65e0\u6cd5\u5199\u5165\uff1a")) +
           L"\n" + path;
}

inline std::wstring RegisterFailed(Lang lang) {
    return Pick(lang,
                L"\u97f3\u58f0\u306e\u767b\u9332\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002\nDllRegisterServer",
                L"Could not register the voices.\nDllRegisterServer failed.",
                L"\u8bed\u97f3\u6ce8\u518c\u5931\u8d25\u3002\nDllRegisterServer \u8fd4\u56de\u9519\u8bef\u3002");
}

inline std::wstring NoVoices(Lang lang) {
    return Pick(lang,
                L"\u767b\u9332\u306f\u5b8c\u4e86\u3057\u307e\u3057\u305f\u304c\u3001\u65e5\u672c\u8a9e\u97f3\u58f0\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093\u3002",
                L"Registration finished, but SAPI still sees no Japanese voice.",
                L"\u6ce8\u518c\u5b8c\u6210\uff0c\u4f46 SAPI \u4ecd\u7136\u770b\u4e0d\u5230\u4efb\u4f55\u65e5\u8bed\u8bed\u97f3\u3002");
}

inline std::wstring InstallDone(Lang lang, int voices, int characters) {
    // Two numbers, because they answer different questions: the voice count is what shows up in
    // the picker, the character count is how many people are behind it. A pack like 0.vvm is
    // 10 voices but only 4 characters, and "10" alone makes the install look bigger than it is.
    wchar_t count[64] = {};
    if (characters > 0) {
        std::swprintf(count, 64,
                      Pick(lang, L"%d \u4ef6\uff08%d \u30ad\u30e3\u30e9\u30af\u30bf\u30fc\uff09",
                           L"%d (%d characters)", L"%d \u6761\uff08%d \u4e2a\u89d2\u8272\uff09"),
                      voices, characters);
    } else {
        std::swprintf(count, 64, L"%d", voices);
    }
    const std::wstring tail =
        Pick(lang,
             L"\n\nChrome / Edge \u3092\u8d77\u52d5\u3057\u76f4\u3059\u3068\u3001\u97f3\u58f0\u4e00\u89a7\u306b\u73fe\u308c\u307e\u3059\u3002",
             L"\n\nRestart Chrome or Edge and the voices show up in the list.",
             L"\n\n\u91cd\u542f Chrome / Edge \u540e\uff0c\u8bed\u97f3\u5217\u8868\u91cc\u5c31\u4f1a\u51fa\u73b0\u5b83\u4eec\u3002");
    return std::wstring(Pick(lang, L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u304c\u5b8c\u4e86\u3057\u307e\u3057\u305f\u3002\n\n\u5229\u7528\u3067\u304d\u308b\u65e5\u672c\u8a9e\u97f3\u58f0: ",
                              L"Installation complete.\n\nJapanese voices available: ",
                              L"\u5b89\u88c5\u5b8c\u6210\u3002\n\n\u53ef\u7528\u65e5\u8bed\u8bed\u97f3\uff1a")) +
           count + tail;
}

/**
 * Shown when the engine DLL was loaded by a browser during the install.
 *
 * <p>Not an error: the voices work right away, only the updated engine is deferred until the
 * machine restarts, because Windows will not unlink a file that is currently mapped. The
 * wording names the usual culprit without claiming to know which process it was.
 */
inline std::wstring RebootNeeded(Lang lang) {
    return Pick(lang,
                L"\u4e00\u90e8\u306e\u30d5\u30a1\u30a4\u30eb\u306f\u5b9f\u884c\u4e2d\u306e\u30d7\u30ed\u30b0\u30e9\u30e0\uff08Chrome / Edge \u306a\u3069\uff09\u304c\n"
                L"\u4f7f\u7528\u4e2d\u306e\u305f\u3081\u3001\u518d\u8d77\u52d5\u5f8c\u306b\u66f4\u65b0\u3055\u308c\u307e\u3059\u3002\n\n"
                L"\u30d6\u30e9\u30a6\u30b6\u3092\u9589\u3058\u3066\u304b\u3089\u3084\u308a\u76f4\u3059\u3068\u3001\u518d\u8d77\u52d5\u305b\u305a\u306b\u6e08\u307f\u307e\u3059\u3002",
                L"Some files are in use by a running program (usually Chrome or Edge),\n"
                L"so they will be replaced the next time Windows starts.\n\n"
                L"To avoid the restart, close the browsers and run this again.",
                L"\u90e8\u5206\u6587\u4ef6\u6b63\u88ab\u8fd0\u884c\u4e2d\u7684\u7a0b\u5e8f\uff08\u901a\u5e38\u662f Chrome / Edge\uff09\u5360\u7528\uff0c\n"
                L"\u5c06\u5728\u4e0b\u6b21\u5f00\u673a\u65f6\u5b8c\u6210\u66ff\u6362\u3002\n\n"
                L"\u5173\u95ed\u6d4f\u89c8\u5668\u540e\u91cd\u65b0\u8fd0\u884c\uff0c\u5c31\u4e0d\u9700\u8981\u91cd\u542f\u3002");
}

/** A payload file whose checksum does not match: a truncated or damaged download. */
inline std::wstring PayloadCorrupt(Lang lang, const std::wstring& path) {
    return std::wstring(Pick(lang,
                             L"インストーラーが破損しています（チェックサムが一致しません）。\n"
                             L"もう一度ダウンロードしてください。\n\n",
                             L"The installer is damaged (checksum mismatch).\nPlease download it again.\n\n",
                             L"安装包已损坏（校验和不匹配）。\n请重新下载。\n\n")) +
           path;
}

inline std::wstring UnregisterFailed(Lang lang) {
    return Pick(lang,
                L"\u767b\u9332\u306e\u89e3\u9664\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002\nChrome / Edge \u3092\u9589\u3058\u3066\u3084\u308a\u76f4\u3057\u3066\u304f\u3060\u3055\u3044\u3002",
                L"Could not unregister.\nClose Chrome or Edge and try again.",
                L"\u89e3\u9664\u6ce8\u518c\u5931\u8d25\u3002\n\u8bf7\u5173\u95ed Chrome / Edge \u540e\u91cd\u8bd5\u3002");
}

inline std::wstring UninstallRestartFailed(Lang lang) {
    return Pick(lang,
                L"\u30a2\u30f3\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3092\u958b\u59cb\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f\u3002",
                L"Could not start the uninstaller copy.",
                L"\u65e0\u6cd5\u542f\u52a8\u5378\u8f7d\u7a0b\u5e8f\u526f\u672c\u3002");
}

inline std::wstring UninstallDone(Lang lang) {
    return Pick(lang,
                L"\u30a2\u30f3\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u304c\u5b8c\u4e86\u3057\u307e\u3057\u305f\u3002\n\n"
                L"Chrome / Edge \u3092\u8d77\u52d5\u3057\u76f4\u3059\u3068\u4e00\u89a7\u304b\u3089\u6d88\u3048\u307e\u3059\u3002",
                L"Uninstalled.\n\nRestart Chrome or Edge to drop the voices from the list.",
                L"\u5378\u8f7d\u5b8c\u6210\u3002\n\n\u91cd\u542f Chrome / Edge \u540e\uff0c\u8bed\u97f3\u4f1a\u4ece\u5217\u8868\u91cc\u6d88\u5931\u3002");
}

/** Uninstalled, but a browser still had some files open; Windows deletes them at next boot. */
inline std::wstring UninstallRebootNeeded(Lang lang) {
    return Pick(lang,
                L"アンインストールしました。使用中のファイルは次回の Windows 起動時に削除されます。",
                L"Uninstalled. Files still in use will be deleted the next time Windows starts.",
                L"已卸载。仍被占用的文件将在下次 Windows 启动时删除。");
}

}  // namespace vvmsg
