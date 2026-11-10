// UI language: Russian (source strings), English, Ukrainian.
// TR("русский текст") / TRW(L"русский текст") return the text in the current language (or the original if untranslated).
#pragma once

namespace phonecam {
enum class Lang { Ru = 0, En = 1, Uk = 2 };
Lang CurrentLang();
void SetLang(Lang l);
Lang LangFromCode(const char* code);  // "ru", "uk-UA", "en-US"... (unknown -> En)
// Language from settings (lang=auto|ru|en|uk in %APPDATA%\PhoneCam\phonecam.ini) or the Windows UI language.
Lang DetectLang();
}  // namespace phonecam

const char* TR(const char* ru);
const wchar_t* TRW(const wchar_t* ru);
