#include "voices_i18n.h"

#include <string>

namespace voicevox {
namespace {

// Names and voice descriptions.
//
// <p>The Japanese names are the keys: they are what the .vvm metadata reports, so a pack that
// ships a character not listed here still works — it just falls back to Japanese instead of
// failing. The English column is the romaji used by the official product pages
// (voicevox.hiroshiba.jp/product/<slug>), and the tags are a compressed translation of the one
// line the site prints under each character, e.g. ずんだもん = 子供っぽい高めの声.
//
// <p>There are no official Chinese names; the zh column is a transliteration in the style
// Chinese fans use (ずんだもん → 俊达萌), so it is a convenience, not a canon.
const CharacterInfo kCharacters[] = {
    {L"四国めたん", L"Shikoku Metan", L"四国梅丹",
     L"明瞭で芯のある声", L"Clear and firm", L"清晰有底气"},
    {L"ずんだもん", L"Zundamon", L"俊达萌",
     L"子供っぽい高めの声", L"Childish and high", L"孩子气高音"},
    {L"春日部つむぎ", L"Kasukabe Tsumugi", L"春日部紬",
     L"明るく元気な声", L"Bright and lively", L"元气明亮"},
    {L"雨晴はう", L"Amehare Hau", L"雨晴哈乌",
     L"優しくかわいい声", L"Gentle and cute", L"温柔可爱"},
    {L"波音リツ", L"Namine Ritsu", L"波音律",
     L"低くクールな声", L"Low and cool", L"低沉稳酷"},
    {L"玄野武宏", L"Kurono Takehiro", L"玄野武宏",
     L"爽やかな青年の声", L"Fresh young man", L"爽朗青年"},
    {L"白上虎太郎", L"Shirakami Kotarou", L"白上虎太郎",
     L"声変わり直後の少年", L"Boy, voice just broke", L"刚变声的少年"},
    {L"青山龍星", L"Aoyama Ryusei", L"青山龙星",
     L"重厚な低音", L"Deep, weighty bass", L"厚重低音"},
    {L"冥鳴ひまり", L"Meimei Himari", L"冥鸣日葵",
     L"柔らかく温かい声", L"Soft and warm", L"柔软温暖"},
    {L"九州そら", L"Kyushu Sora", L"九州空",
     L"上品な大人の声", L"Refined, adult", L"成熟有品"},
    {L"もち子さん", L"Mochiko-san", L"饼子小姐",
     L"明瞭で穏やかな声", L"Clear and calm", L"清晰平稳"},
    {L"剣崎雌雄", L"Kenzaki Mesuo", L"剑崎雌雄",
     L"落ち着いた安心感", L"Calm, reassuring", L"沉稳安心"},
    {L"WhiteCUL", L"WhiteCUL", L"WhiteCUL",
     L"素直で聞きやすい声", L"Frank, easy on the ear", L"直率顺耳"},
    {L"後鬼", L"Goki", L"后鬼",
     L"包容力のある静けさ", L"Embracing, quiet", L"包容娴静"},
    {L"No.7", L"No.7", L"No.7",
     L"しっかり凛々しい声", L"Steady and gallant", L"扎实凛然"},
    {L"ちび式じい", L"Chibishiki-jii", L"迷你式爷爷",
     L"しゃがれ声の親しみ", L"Friendly, gravelly", L"沙哑亲切"},
    {L"櫻歌ミコ", L"Ouka Miko", L"樱歌美子",
     L"かわいい少女の声", L"Cute young girl", L"可爱少女"},
    {L"小夜/SAYO", L"Sayo", L"小夜/SAYO",
     L"穏やかで優しい声", L"Gentle, good-natured", L"温和宽厚"},
    {L"ナースロボ＿タイプＴ", L"Nurse Robo Type T", L"护士机器人T型",
     L"冷静で控えめな声", L"Calm, reserved", L"冷静克制"},
    {L"†聖騎士 紅桜†", L"Holy Knight Benizakura", L"†圣骑士 红樱†",
     L"快活で歯切れのよい声", L"Cheerful and crisp", L"爽快干脆"},
    {L"雀松朱司", L"Wakamatsu Akashi", L"雀松朱司",
     L"静かで着実な声", L"Quiet and steady", L"安静稳重"},
    {L"麒ヶ島宗麟", L"Kigashima Sourin", L"麒之岛宗麟",
     L"渋い中年の声", L"Mellow middle-aged man", L"有味道的大叔"},
    {L"春歌ナナ", L"Haruka Nana", L"春歌娜娜",
     L"元気で力強い声", L"Lively and powerful", L"活泼有力"},
    {L"猫使アル", L"Nekotsuka Aru", L"猫使阿尔",
     L"温かくおおらかな声", L"Warm and easygoing", L"厚实随和"},
    {L"猫使ビィ", L"Nekotsuka Bii", L"猫使比",
     L"純真無垢な声", L"Pure and innocent", L"纯真天真"},
    {L"中国うさぎ", L"Chugoku Usagi", L"中国兔",
     L"幻想的で初々しい声", L"Ethereal, fresh", L"幽玄青涩"},
    {L"栗田まろん", L"Kurita Maron", L"栗田玛珑",
     L"深みのある中性的な声", L"Deep, androgynous", L"中性低沉"},
    {L"あいえるたん", L"Aierutan", L"艾尔炭",
     L"心地よく柔らかい声", L"Pleasant and gentle", L"舒适柔和"},
    {L"満別花丸", L"Manbetsu Hanamaru", L"满别花丸",
     L"鮮やかで個性的な声", L"Vivid, standout", L"生动突出"},
    {L"琴詠ニア", L"Kotoyomi Nia", L"琴咏妮娅",
     L"滑らかで無機質な声", L"Smooth, inorganic", L"顺滑无机质"},
    {L"Voidoll", L"Voidoll", L"Voidoll",
     L"控えめな電子音声", L"Modest, electronic", L"谦逊电子音"},
    {L"ぞん子", L"Zonko", L"尊子",
     L"熱血で真っ直ぐな声", L"Hot-blooded, vivid", L"热血真切"},
    {L"中部つるぎ", L"Chubu Tsurugi", L"中部剑",
     L"凛として堂々とした声", L"Dignified, commanding", L"凛然有威"},
    {L"離途", L"Rito", L"离途",
     L"息をまとった声", L"Wrapped in breath", L"气声包裹"},
    {L"黒沢冴白", L"Kurosawa Kohaku", L"黑泽冴白",
     L"強気で張りのある声", L"Assertive, resonant", L"强势有张力"},
    {L"ユーレイちゃん", L"Yurei-chan", L"幽灵酱",
     L"柔らかく漂う声", L"Soft, drifting", L"柔和飘荡"},
    {L"東北ずん子", L"Tohoku Zunko", L"东北俊子",
     L"しとやかで親しみやすい声", L"Graceful, friendly", L"娴静亲和"},
    {L"東北きりたん", L"Tohoku Kiritan", L"东北切蒲英",
     L"清らかで控えめな声", L"Crisp, modest", L"清丽内敛"},
    {L"東北イタコ", L"Tohoku Itako", L"东北伊达子",
     L"優雅な余韻のある声", L"Elegant, resonant", L"优雅余韵"},
    {L"あんこもん", L"Ankomon", L"馅子怪",
     L"抑えた負けん気の声", L"Reserved but competitive", L"收敛好胜"},
    {L"夜語トバリ", L"Yogatari Tobari", L"夜语帐",
     L"理知的で明瞭な声", L"Intellectual, clear", L"理性清晰"},
    {L"暁記ミタマ", L"Akatsuki Mitama", L"晓记御魂",
     L"儚く漂う声", L"Faint, floating", L"虚幻漂浮"},
    {L"里石ユカ", L"Satoishi Yuka", L"里石由香",
     L"つぼみのような初々しさ", L"Young, budding", L"含苞清纯"},
    // The generic voices in n0.vvm have no character attached to them.
    {L"男声1", L"Male 1", L"男声1", L"汎用の男声", L"Generic male", L"通用男声"},
    {L"男声2", L"Male 2", L"男声2", L"汎用の男声", L"Generic male", L"通用男声"},
    {L"男声3", L"Male 3", L"男声3", L"汎用の男声", L"Generic male", L"通用男声"},
    {L"女声1", L"Female 1", L"女声1", L"汎用の女声", L"Generic female", L"通用女声"},
    {L"女声2", L"Female 2", L"女声2", L"汎用の女声", L"Generic female", L"通用女声"},
    {L"女声3", L"Female 3", L"女声3", L"汎用の女声", L"Generic female", L"通用女声"},
    {L"女声4", L"Female 4", L"女声4", L"汎用の女声", L"Generic female", L"通用女声"},
    {L"女声5", L"Female 5", L"女声5", L"汎用の女声", L"Generic female", L"通用女声"},
    {L"女声6", L"Female 6", L"女声6", L"汎用の女声", L"Generic female", L"通用女声"},
};

// Style names are a closed vocabulary shared across characters, so they get their own table
// instead of being repeated per character.
const StyleInfo kStyles[] = {
    {L"ノーマル", L"Normal", L"普通"},
    {L"ふつう", L"Normal", L"普通"},
    {L"あまあま", L"Sweet", L"甜甜"},
    {L"甘々", L"Sweet", L"甜甜"},
    {L"ツンツン", L"Tsundere", L"傲娇"},
    {L"ツンギレ", L"Angry Tsundere", L"炸毛傲娇"},
    {L"セクシー", L"Sexy", L"性感"},
    {L"セクシー／あん子", L"Sexy / Anko", L"性感／馅子"},
    {L"ささやき", L"Whisper", L"低语"},
    {L"囁き", L"Whisper", L"耳语"},
    {L"ヒソヒソ", L"Whispering", L"窃窃私语"},
    {L"内緒話", L"Secret Whisper", L"悄悄话"},
    {L"わーい", L"Yay", L"欢呼"},
    {L"たのしい", L"Happy", L"开心"},
    {L"うきうき", L"Cheerful", L"雀跃"},
    {L"元気", L"Energetic", L"元气"},
    {L"明るい", L"Bright", L"明朗"},
    {L"のんびり", L"Easygoing", L"悠闲"},
    {L"おちつき", L"Calm", L"沉稳"},
    {L"しっとり", L"Mellow", L"温婉"},
    {L"けだるげ", L"Listless", L"慵懒"},
    {L"びくびく", L"Nervous", L"战战兢兢"},
    {L"おどおど", L"Timid", L"怯生生"},
    {L"こわがり", L"Fearful", L"胆小"},
    {L"人見知り", L"Shy", L"怕生"},
    {L"おどろき", L"Surprised", L"惊讶"},
    {L"おこ", L"Angry", L"生气"},
    {L"怒り", L"Anger", L"愤怒"},
    {L"不機嫌", L"Grumpy", L"不高兴"},
    {L"呆れ", L"Dumbfounded", L"无语"},
    {L"びえーん", L"Wailing", L"大哭"},
    {L"泣き", L"Crying", L"哭泣"},
    {L"なみだめ", L"Teary", L"泪眼"},
    {L"かなしい", L"Sad", L"难过"},
    {L"かなしみ", L"Sadness", L"哀伤"},
    {L"悲しみ", L"Sorrow", L"悲恸"},
    {L"哀しみ", L"Sorrow", L"哀伤"},
    {L"喜び", L"Joy", L"喜悦"},
    {L"熱血", L"Hot-blooded", L"热血"},
    {L"シリアス", L"Serious", L"严肃"},
    {L"実況風", L"Play-by-play", L"实况解说风"},
    {L"アナウンス", L"Announcement", L"播报"},
    {L"読み聞かせ", L"Storytelling", L"朗读"},
    {L"第二形態", L"Second Form", L"第二形态"},
    {L"覚醒", L"Awakened", L"觉醒"},
    {L"絶望と敗北", L"Despair and Defeat", L"绝望与败北"},
    {L"ロリ", L"Loli", L"萝莉"},
    {L"ボーイ", L"Boy", L"少年"},
    {L"ぶりっ子", L"Coy", L"装可爱"},
    {L"クイーン", L"Queen", L"女王"},
    {L"つよつよ", L"Strong", L"强强"},
    {L"よわよわ", L"Weak", L"弱弱"},
    {L"へろへろ", L"Exhausted", L"软塌塌"},
    {L"ヘロヘロ", L"Exhausted", L"软塌塌"},
    {L"低血圧", L"Low Blood Pressure", L"低血压"},
    {L"楽々", L"Relaxed", L"轻松"},
    {L"恐怖", L"Terror", L"恐惧"},
    {L"つぼみ", L"Bud", L"花蕾"},
    {L"ツクモちゃん", L"Tsukumo-chan", L"九十九酱"},
    {L"人間ver.", L"Human ver.", L"人类版"},
    {L"人間（怒り）ver.", L"Human (Angry) ver.", L"人类（愤怒）版"},
    {L"鬼ver.", L"Oni ver.", L"鬼版"},
    {L"ぬいぐるみver.", L"Plushie ver.", L"玩偶版"},
    {L"男声1", L"Male 1", L"男声1"},
    {L"男声2", L"Male 2", L"男声2"},
    {L"男声3", L"Male 3", L"男声3"},
    {L"女声1", L"Female 1", L"女声1"},
    {L"女声2", L"Female 2", L"女声2"},
    {L"女声3", L"Female 3", L"女声3"},
    {L"女声4", L"Female 4", L"女声4"},
    {L"女声5", L"Female 5", L"女声5"},
    {L"女声6", L"Female 6", L"女声6"},
};

}  // namespace

UiLang SystemUiLang() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE:
            return UiLang::Japanese;
        case LANG_CHINESE:
            return UiLang::Chinese;
        default:
            return UiLang::English;
    }
}

const CharacterInfo* FindCharacter(const wchar_t* ja) {
    if (ja == nullptr) return nullptr;
    for (const CharacterInfo& info : kCharacters) {
        if (wcscmp(info.ja, ja) == 0) return &info;
    }
    return nullptr;
}

const StyleInfo* FindStyleName(const wchar_t* ja) {
    if (ja == nullptr) return nullptr;
    for (const StyleInfo& info : kStyles) {
        if (wcscmp(info.ja, ja) == 0) return &info;
    }
    return nullptr;
}

const wchar_t* CharacterName(const wchar_t* ja, UiLang lang) {
    const CharacterInfo* info = FindCharacter(ja);
    if (info == nullptr) return ja;
    switch (lang) {
        case UiLang::English:
            return info->en;
        case UiLang::Chinese:
            return info->zh;
        default:
            return info->ja;
    }
}

const wchar_t* StyleName(const wchar_t* ja, UiLang lang) {
    const StyleInfo* info = FindStyleName(ja);
    if (info == nullptr) return ja;
    switch (lang) {
        case UiLang::English:
            return info->en;
        case UiLang::Chinese:
            return info->zh;
        default:
            return info->ja;
    }
}

const wchar_t* CharacterTag(const wchar_t* ja, UiLang lang) {
    const CharacterInfo* info = FindCharacter(ja);
    if (info == nullptr) return L"";
    switch (lang) {
        case UiLang::English:
            return info->tagEn;
        case UiLang::Chinese:
            return info->tagZh;
        default:
            return info->tagJa;
    }
}

std::wstring LocalizedDisplayName(const wchar_t* character, const wchar_t* style, UiLang lang) {
    std::wstring name = std::wstring(CharacterName(character, lang)) + L" / " + StyleName(style, lang);
    const wchar_t* tag = CharacterTag(character, lang);
    if (tag != nullptr && tag[0] != L'\0') {
        // U+00B7 MIDDLE DOT, so the tag reads as an aside and not as part of the style name.
        name += L" \u00b7 ";
        name += tag;
    }
    return name;
}

const wchar_t* LanguageValueName(UiLang lang) {
    // SAPI looks the description up under a value named after the UI language's LCID in hex,
    // then falls back to the key's default value. Chrome and Narrator do exactly this.
    switch (lang) {
        case UiLang::Japanese:
            return L"0411";
        case UiLang::Chinese:
            return L"0804";
        default:
            return L"0409";
    }
}

}  // namespace voicevox
