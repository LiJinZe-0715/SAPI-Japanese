// Shared icon + VERSIONINFO block. Each .rc in this directory defines the four macros below and
// then includes this file, so the version numbers and the product name exist in exactly one place.
//
// Everything here is ASCII on purpose: rc.exe reads sources in the system code page, and the
// product name carries Japanese. Keep localised strings in tools/messages.h, where they are
// already maintained for three languages.

#ifndef VV_DESCRIPTION
#error "define VV_DESCRIPTION, VV_INTERNAL, VV_ORIGINAL and VV_FILETYPE before including this"
#endif

// ID 1: Windows picks the lowest-numbered icon as the one Explorer and the taskbar show.
1 ICON "app.ico"

VS_VERSION_INFO VERSIONINFO
    FILEVERSION 1, 0, 0, 0
    PRODUCTVERSION 1, 0, 0, 0
    FILEFLAGSMASK 0x3fL
    FILEFLAGS 0x0L
    FILEOS VOS_NT_WINDOWS32
    FILETYPE VV_FILETYPE
    FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        // 0x0409 / 1200 = US English, Unicode. The strings are not translated, so there is no
        // reason to duplicate the block per language.
        BLOCK "040904B0"
        BEGIN
            VALUE "CompanyName", "VOICEVOX SAPI contributors"
            VALUE "FileDescription", VV_DESCRIPTION
            VALUE "FileVersion", "1.0.0.0"
            VALUE "InternalName", VV_INTERNAL
            VALUE "LegalCopyright", "VOICEVOX Core and the voice models remain the property of their respective authors."
            VALUE "OriginalFilename", VV_ORIGINAL
            VALUE "ProductName", "VOICEVOX SAPI"
            VALUE "ProductVersion", "1.0.0.0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
