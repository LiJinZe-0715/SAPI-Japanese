# SAPI-Japanese

把 VOICEVOX 注册成 Windows 的**系统语音**（SAPI5）。装完之后，Chrome、Edge、讲述人，以及任何
走 SAPI 的程序，在语音下拉列表里就能直接看到「俊达萌 / 普通 · 孩子气高音」这样的条目。

合成在**调用进程内**完成：直接加载 `voicevox_core.dll`，不需要启动 VOICEVOX 编辑器，也不经过
`127.0.0.1:50021` 的 HTTP 服务。

- 支持 Windows 10 / 11，64 位
- 语音名称按系统语言显示中文、日文或英文
- 安装包自带运行时和模型，装的时候不需要联网

## 安装

下载 `VoicevoxSapiSetup.exe`，双击，允许 UAC，几秒后提示安装完成，然后**重启浏览器**。

装完之后，语音会出现在这些地方：

| 位置 | 说明 |
|---|---|
| 设置 → 时间和语言 → 语音 | Windows 11 的语音列表 |
| 控制面板 → 语音识别 → 文本到语音 | 传统 SAPI5 列表 |
| Chrome 控制台 `speechSynthesis.getVoices()` | 网页 TTS |
| 讲述人、Balabolka 等 SAPI 程序 | 直接可选用 |

安装本身不需要联网。如果机器上缺 VC++ 运行库，`voicevox_core.dll` 会加载失败，先装一次
[VC++ 2015-2022 x64 运行库](https://aka.ms/vs/17/release/vc_redist.x64.exe)。

### 三种安装包

| 模式 | 模型包 | 装完的语音 | 体积 |
|---|---|---|---|
| `generic`（默认） | `n0.vvm` | 女声1/2/3 + 男声1/2/3，共 **6 条** | 约 193 MB |
| `classic` | `0.vvm` | 四国めたん / ずんだもん / 春日部つむぎ / 雨晴はう，4 个角色 **10 条** | 约 178 MB |
| `all` | 全部 27 个 `.vvm` | **136 条**，52 个角色 | 约 1.8 GB |

`all` 会连歌唱包 `s0.vvm` 一起带上。它只有歌唱风格，不产生语音条目。

`generic` 里的「女声3」用的是女声6 的声音，名字保持「女声3」，见「选择装哪些语音」。

一个安装包同时决定**注册哪些语音**和**机器上保留哪些模型**：装 `generic` 时，`models\` 目录里
不属于这个包的 `.vvm` 会被删除。所以装完之后，一个包就是一套语音，不会残留上一套模型。

### 装到哪

| 路径 | 内容 |
|---|---|
| `%ProgramFiles%\VoicevoxSapi\` | `VoicevoxSapi.dll`、`VoicevoxSapiUninstall.exe`、`voicevox-sapi-setup.exe` |
| `%ProgramData%\VoicevoxSapi\` | `voicevox_core.dll`、`voicevox_onnxruntime.dll`、`dict\`、`voices.txt` |
| `%ProgramData%\VoicevoxSapi\models\` | 模型包 `*.vvm` |
| `%ProgramData%\VoicevoxSapi\voicevox-sapi.log` | 运行日志 |

## 语音名称

语音列表里的一条长这样：

```
俊达萌 / 普通 · 孩子气高音
ずんだもん / ノーマル · 子供っぽい高めの声     ← 日文系统
Zundamon / Normal · Childish and high         ← 英文系统
```

三段分别是**角色 / 风格 · 音质**。音质那一句取自 voicevox.hiroshiba.jp 每个角色页面上的一行
说明，压到能塞进名字里的长度。名字是多数宿主唯一会显示的信息，只写「四国梅丹 / 普通」看不出这个
声音的特点。

名字在注册时一次写三份，SAPI 按当前界面语言挑一份显示，换系统语言不需要重新安装：

| 注册表值名 | 给谁看 |
|---|---|
| 默认值 | 回退用（同时写进 `Attributes\Name`） |
| `0411` | 日文系统 |
| `0409` | 英文系统 |
| `0804` | 中文系统 |

全部 136 条语音的中日英对照表在 **`docs/voices.html`**，可搜索，三列并排。它和 DLL 编译的是同一份
名称表，所以页面和语音列表不会各说各话。

中文名没有官方译名，用的是中文圈的习惯写法（ずんだもん → 俊达萌），是方便，不是标准。表里没有的
角色不会报错，直接退回日文原名，新模型包加进来时走的就是这条路径。

## 选择装哪些语音

装哪些语音由 `%ProgramData%\VoicevoxSapi\voices.txt` 决定，一行一条，写 style id 或角色名
（中日英都认）。引擎注册时只写里面列的：

```
# generic voices only: female 1-3, male 1-3
# 10006 = 10004 registers female 6 under female 3's name
10000
10001
10002
10005
10007
10006 = 10004
```

`=` 的左边是「选哪条」，右边是「显示成什么」：

- 右边写另一个 style id，就借用它的名字。上面最后一行注册的是女声6（10006），显示名用女声3
  （10004）的，所以列表里是「女声3 / 普通 · 通用女声」，发声的是女声6
- 右边写文字，就是字面标签，只替换角色名，风格和音质照旧

改完文件重跑一次安装即可生效。`voicevox-sapi-setup.exe` 随安装包装在
`C:\Program Files\VoicevoxSapi\`，仓库里 `build\` 下也有同样的副本：

```powershell
voicevox-sapi-setup.exe install                       # 按 voices.txt 注册
voicevox-sapi-setup.exe install --only "女声1,男声1"   # 直接指定，写进 voices.txt
voicevox-sapi-setup.exe install --all                 # 删掉 voices.txt，全注册
```

注册是**整批替换**而不是追加：不再被选中的语音、以及模型包已经不在的语音，令牌会在同一次注册里
被删掉，列表不会残留。

`voices.txt` 用 UTF-8 保存即可，带不带 BOM 都能读。整份文件只有注释、或者文件不存在，都表示
「不筛选」，会把在位模型包里的语音全部注册。

想加语音，把对应的 `.vvm` 放进 `models\`，再跑一次 `install`，不需要重新打包或编译。
注意重跑**安装包**会按它自己的清单删掉多出来的模型，手工加模型请用命令行 `install`。

## 卸载

**普通用户**：设置 → 应用和功能 → 「VOICEVOX 音声 (SAPI5)」→ 卸载。卸载会移除语音令牌、程序目录
和运行时，卸载完重启浏览器，语音从列表里消失。

**命令行**：

```powershell
voicevox-sapi-setup.exe uninstall          # 反注册 + 删程序目录，保留运行时和模型
voicevox-sapi-setup.exe uninstall --purge  # 连运行时和模型一起删
VoicevoxSapiSetup.exe --uninstall          # 用安装包本体卸载（备用入口）
```

## 常见问题

**为什么我的列表里有 136 条，别人装完只有 6 条？**
语音条数取决于 `models\` 里放了多少个模型包，不是安装器写死的。136 条是 26 个含 talk 风格的模型包
全部就位时的总数；`generic` 只带一个 `n0.vvm`，所以是 6 条。补模型后重跑 `install` 即可。

**能不能只保留某几个角色，把 `.vvm` 拆开？**
不能，也没有意义。`.vvm` 是一个 zip，里面是 4 个共享的 ONNX（`pd` / `pi` / `gfi` / `ras`）加
`metas.json`、`manifest.json`。一个包里所有角色**共用同一套权重**，角色只是 `metas.json` 里的
style id。所以没有「单个角色」的文件可拆，拆了也省不下空间 —— `n0.vvm` 的 72 MB 里 57 MB 是
那个共享声学模型。

要「只保留几条」控制的是**注册**，不是模型文件，用上面的 `voices.txt` 就够了。

**浏览器里没声音。**
先看 `%ProgramData%\VoicevoxSapi\voicevox-sapi.log`。正常朗读会留下 `speak: style=N` 和
`speak: first audio after N ms`；没有这两行说明请求没到引擎，检查语音是否在列表里、浏览器是否重启过。
有这两行但没声音，多半是运行库缺失或模型包被删。

**音高能调吗？**
不能。SAPI 的引擎接口 `ISpTTSEngineSite` 只有 `Write` / `GetRate` / `GetVolume` / `GetSkipInfo` /
`CompleteSkip`，没有 pitch 通道，所以 `pitchScale` 恒为中性值。语速和音量可以调。

**升级时浏览器开着可以吗？**
可以。引擎 DLL 被浏览器映射时无法直接覆盖，安装器会先把新文件写到旁边再改名顶掉；改名也被拒绝时
挂到下次开机替换，语音当场就能用，只有引擎二进制晚一次开机生效。重复安装不会产生多余的待替换记录。

**32 位程序能用吗？**
不能。voicevox_core 只发布 x86_64 版本，32 位进程里没有可加载的 `voicevox_core.dll`。

## 技术说明

| 层 | 说明 |
|---|---|
| `VoicevoxSapi.dll` | 进程内 COM 服务器，实现 `ISpTTSEngine` + `ISpObjectWithToken`，被宿主进程加载 |
| 一个角色 = 一个 CLSID | style id 编在 CLSID 里（`Data1` 低字节 + `Data4[3]` 高字节），`DllGetClassObject` 反解，一个 DLL 服务所有角色 |
| 合成 | `voicevox_synthesizer_create_audio_query` → 改 scale → `voicevox_synthesizer_synthesis` → 剥 RIFF 头 → 按 8 KB 分块 `Write` |
| 流式 | 按句末标点切分，逐句合成逐句写出，首字延迟等于第一句的合成时间 |
| 输出格式 | 24 kHz / 16 bit / 单声道，与 `GetOutputFormat` 声明一致 |
| 内存 | 模型包懒加载，最近使用的 4 个常驻，超出按 LRU 卸载 |

SAPI 把引擎加载进**调用方进程**，所以引擎崩溃会带走宿主；所有失败路径都静默返回 `S_OK`（不发声），
错误原因记在 `Core::LastError()` 和日志里。

### 注册要求

以下三条是 SAPI 的硬性要求，改动注册或引擎代码时需要注意。

**必须实现 `ISpObjectWithToken`。** SAPI 在拿到 `GetOutputFormat` 之后，会再
`QueryInterface(IID_ISpObjectWithToken)` 把语音令牌交给引擎。这里返回 `E_NOINTERFACE`，
`ISpVoice::Speak` 会以 `SPERR_VOICE_NOT_FOUND`（`0x80045093`）失败，即使 `ISpTTSEngine`
已经 QI 成功、`GetOutputFormat` 也正常调用过。

**语音令牌要注册到两个类别。** Chrome、Edge、讲述人先枚举
`HKLM\SOFTWARE\Microsoft\Speech_OneCore\Voices`，只有该类别枚举失败时才回退到传统的
`HKLM\SOFTWARE\Microsoft\Speech\Voices`。正常 Windows 上 OneCore 类别永远非空，所以只注册传统
类别的语音在浏览器里完全不可见，但 SAPI 程序仍然能用。因此两个根都要写。

**`Attributes\Language` 必须是 `411`（ja-JP）。** 写成 `804` 是 zh-CN，SAPI 会把语音归到中文下面，
Chrome 里显示成 `zh-CN`。

### 参数换算

SAPI 与 VOICEVOX 的中性值不同，每个值都要换算：

| SAPI | 范围 / 中性 | VOICEVOX | 换算 |
|---|---|---|---|
| `GetRate()` | -10..10 / 0 | `speedScale` 1.0 中性 | `2^(rate/10)`，夹到 0.5~2.0 |
| `GetVolume()` | 0..100 / 100 | `volumeScale` 1.0 中性 | `volume/100` |
| —— | 无 pitch 通道 | `pitchScale` **0.0** 中性 | 固定 0.0 |

`pitchScale` 是唯一以 0.0 为中性的参数（其余都是 1.0）。把它当 1.0 传会让每个音节升高一个八度，
输出变成噪音。

## 构建

### 先准备外部文件

本仓库**只有源码**（不到 1 MB）：没有 `.vvm`、没有 `voicevox_core.dll`、没有任何模型。
那几样加起来 1.6 GB，从 VOICEVOX 官方仓库下载，不要提交进本仓库。

需要两份：

**1. voicevox_core 发行包** —— 编译时要它的头文件和导入库
[github.com/VOICEVOX/voicevox_core/releases](https://github.com/VOICEVOX/voicevox_core/releases)

下载 `voicevox_core-windows-x64-<版本>.zip`，解压到任意目录（例如 `third_party\voicevox_core`）。
解压后应该是这样：

```
<core>\
  c_api\include\voicevox_core.h          <- 编译要
  c_api\lib\voicevox_core.lib            <- 链接要
  c_api\lib\voicevox_core.dll            <- 运行要
  onnxruntime\lib\voicevox_onnxruntime.dll
  dict\open_jtalk_dic_utf_8-1.11\
  models\vvms\                           <- 自己建，第 2 份文件放这里
```

**2. `.vvm` 模型包** —— 打包和运行需要，**编译不需要**
[github.com/VOICEVOX/voicevox_vvm/releases](https://github.com/VOICEVOX/voicevox_vvm/releases)

每个 `.vvm` 是一个独立的 release 附件，逐个下载后放进 `<core>\models\vvms\`。文件名开头表示类型：
数字开头是说话用（`0.vvm`、`6.vvm`），`s` 开头是歌唱用（`s0.vvm`），`n` 开头是 VOICEVOX Nemo
（`n0.vvm`）。至少要 `n0.vvm`（`generic` 包）或 `0.vvm`（`classic` 包）。

编译之所以不需要 `.vvm`：角色表 `src/catalog_table.inc` 和名称表是**生成好放在仓库里**的，
`build.bat` 不读模型文件。只有 `package.bat` 要把 `.vvm` 打进安装包、`voicevox-model-metas`
要读元数据时才需要它们。

`.vvm` 受 [VOICEVOX 音声モデル利用規約](https://github.com/VOICEVOX/voicevox_vvm/blob/main/README.md)
约束，二次分发时按对应角色的条款署名。这也是它们不跟着本仓库走的原因之一。

本仓库的代码对应 **core 0.17.1 + vvm 0.17.0**。换用别的版本时 style id 可能增减，需要重新生成
角色表（见「角色表是生成的」），否则会出现「某个角色用别人的嗓子说话」。

构建时按这个顺序找 core 目录（`build.bat` 和 `package.bat` 规则相同）：

```
命令行参数    >    %VOICEVOX_CORE_DIR%    >    <仓库>\third_party\voicevox_core
```

```powershell
.\build.bat D:\voicevox_core
# 或者用环境变量
$env:VOICEVOX_CORE_DIR = "D:\voicevox_core"
.\build.bat
```

目录里找不到 `c_api\include\voicevox_core.h` 时，CMake 会停下来并把上面两个下载地址打出来。

### 编译

```powershell
.\build.bat
# 产物：build\VoicevoxSapi.dll、voicevox-sapi-setup.exe、voicevox-sapi-check.exe
#       build\VoicevoxSapiSetup.exe（未附加 payload，单独运行会提示不是完整安装包）

.\package.bat            # generic，6 条通用声
.\package.bat classic    # 4 个角色 / 10 条语音
.\package.bat all        # 全部 136 条
.\package.bat generic D:\path\to\voicevox_core   # 换一份 voicevox_core 解压目录
# 产物：dist\VoicevoxSapiSetup.exe
```

**工具链是探测的，不是写死的。** `build.bat` 在运行时找 Visual Studio（先问 `vswhere`，再找
BuildTools / Community 的常见位置）、取它下面最新的 MSVC 工具集和最新的 `10.*` Windows SDK，
然后打印这次选中的版本：

```
[build] Visual Studio  C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
[build] MSVC toolset   ...\VC\Tools\MSVC\14.44.35207
[build] Windows SDK    10.0.26100.0
```

版本不对或者装了不止一套时，用环境变量指定：`VSINSTALLDIR` / `MSVCVER` / `WINSDKVER`。
找不到就报错并说明缺什么，不会一路编到一半再失败。

`build.bat` 手工拼 MSVC / SDK 环境而不调用 `vcvars64.bat`，因为后者内部会调用 `reg.exe`，在限制
`reg.exe` 的环境里会中途退出，导致 `rc.exe not found`。

**打包前会校验角色表。** `src/catalog_table.inc` 是从 `.vvm` 生成的，提交进仓库是为了让编译
不依赖模型包。代价是模型包版本一变，style id 可能重新编号，而这个表不会自己更新 —— 结果是每个
名字都还能用，但发出来的是另一个角色的声音，全程不报错。所以 `package.bat` 会用磁盘上的
`.vvm` 重新生成一份并比对，对不上就拒绝打包并给出重新生成的命令。

**构建可复现。** Release 构建加了 `/Brepro`，同一份源码在同一个目录里连编两次，产物逐字节相同
（`package.bat` 的输出也一样），所以「这次重编到底改没改东西」可以直接用哈希回答。换一个目录
重建时会有几十个字节的差异，那是链接器记录的输入路径哈希，不影响功能。

### 版本号

版本号没有单一来源，是两处手写的常量。改版本时要**同时**改这两处，否则文件属性里的版本和
「应用和功能」里显示的对不上，而且两处都能编过、都能装上，不会报任何错。

| 文件 | 要改的行 | 显示在哪 |
|---|---|---|
| `src/res/version_info.inc` | `FILEVERSION`、`PRODUCTVERSION`、`VALUE "FileVersion"`、`VALUE "ProductVersion"`，共 4 行 | 四个二进制的文件属性（右键 → 详细信息） |
| `tools/install.h` | `constexpr wchar_t kVersion[]` | 注册表 `DisplayVersion`，即「应用和功能」里的版本 |

格式有两种写法：资源块里的数字用逗号分四段（`FILEVERSION 1, 0, 0, 0`），字符串用点分
（`"1.0.0.0"`）。`version_info.inc` 被 `src/res/` 下的 4 个 `.rc` 共享，所以改一次四个二进制
同时生效。改完要重新跑 `package.bat`，否则 `dist/` 里还是旧版本。

**模型版本不记录在产物里。** 安装包带哪一版 `voicevox_core` 和 `.vvm`，完全由 `package.bat` 的
core 目录决定，VERSIONINFO 和日志里都没有。当前这一版对应 core 0.17.1 + vvm 0.17.0，
见「先准备外部文件」。所以只看一个装好的包，是查不出它带的是哪版模型的。

### 单文件安装包

`tools/pack-payload.cpp` 把运行时、词典、模型直接追加在 exe 的 PE 镜像后面，尾部写一个 `VVSPAYL1`
页脚指向文件表。不压缩：ONNX 模型和 `sys.dic` 基本压不动，而压缩流里坏一个字节会导致整个安装失败，
而不是单个文件复制失败。

### 角色表是生成的

一个角色对应一个 style id，而 style id 猜错不会报错，只会让这个角色用别人的嗓子说话。所以角色表
从 `.vvm` 的元数据生成，不手写：

```bash
# voicevox_core.dll 需要在 PATH 上，或放在工具旁边
./build/voicevox-model-metas.exe "C:\path\to\voicevox_core\models\vvms" --cpp > src/catalog_table.inc
./build.bat
```

`--cpp` 输出 `{角色, 风格, style id, 模型包, 是否女声}`，只保留 `talk` 类风格（`s0.vvm` 里是歌唱
风格，SAPI 引擎只说话，用不上）。`--json` 打印原始元数据，不带参数时打印人读的对照表。

当前覆盖 27 个 `.vvm`（26 个含 talk 风格）、136 条语音、52 个角色，其中 `n0.vvm` 贡献 9 个通用
男女声（id 10000+）。style id 是 16 位的：`Data1` 低字节 + `Data4[3]` 高字节。

### 图标

四个对外二进制（`VoicevoxSapi.dll`、`voicevox-sapi-setup.exe`、`VoicevoxSapiSetup.exe`、
`VoicevoxSapiUninstall.exe`）共用同一枚图标和同一份 `VERSIONINFO`，资源在 `src/res/`。

图标由 `tools/make-icon.cpp` 按几何参数生成，16 到 256 共九个尺寸，4×4 超采样抗锯齿：

```bash
./build/voicevox-make-icon.exe src/res/app.ico   # 改了参数之后跑一次，然后提交 .ico
```

`.ico` 是提交进仓库的，构建不会重新生成：`rc.exe` 在编译任何东西之前就需要这个文件存在。
24 px 以下会丢掉三根音量条，因为 16 px 里一根条只有 1.4 px 宽，三根加间隙会糊成一片。

「应用和功能」里的图标来自卸载项的 `DisplayIcon`，指向 `VoicevoxSapi.dll`，所以图标必须编进 DLL。

### 目录

```
src/catalog.cpp          角色表入口 + CLSID 编解码
src/catalog_table.inc    角色表本体，由 tools/model-metas.cpp 生成，不要手改
src/voices_i18n.cpp      角色/风格/音质的中日英名称表（注册表和对照表共用）
src/core.cpp             VOICEVOX C API 封装（单锁串行，懒加载，模型 LRU）
src/engine.cpp           ISpTTSEngine：GetOutputFormat / Speak；ISpObjectWithToken
src/registry.cpp         CLSID 与 SAPI 语音令牌的注册/反注册
src/dllmain.cpp          DllGetClassObject / DllRegisterServer / delay-load 钩子
src/log.cpp              写 %ProgramData%\VoicevoxSapi\voicevox-sapi.log
src/res/                 图标 + VERSIONINFO
tools/install.h          两个安装器共用的复制/删除/注册/卸载逻辑
tools/messages.h         安装器/卸载器的三语提示文本
tools/installer.cpp      VoicevoxSapiSetup：单文件静默安装器
tools/uninstaller.cpp    VoicevoxSapiUninstall：图形卸载程序
tools/pack-payload.cpp   voicevox-pack-payload：把 payload 追加到 exe 后面
tools/setup.cpp          voicevox-sapi-setup：命令行安装/卸载/状态
tools/sapi-check.cpp     voicevox-sapi-check：不开浏览器直接验证
tools/model-metas.cpp    voicevox-model-metas：读 .vvm 元数据生成角色表
tools/voices-page.cpp    voicevox-voices-page：生成 docs\voices.html
tools/make-icon.cpp      voicevox-make-icon：生成 src\res\app.ico
tools/check-chrome.mjs   用 CDP 让真 Chrome 枚举并朗读
docs/voices.html         136 条语音的中日英对照表（生成的）
CMakeLists.txt           构建定义
build.bat                拼好 MSVC 环境后调 CMake + Ninja
package.bat              打 dist\VoicevoxSapiSetup.exe
third_party/             voicevox_core 和 .vvm 的推荐落脚处（不跟踪，见「先准备外部文件」）
```

## 开发工具

### voicevox-sapi-check

不打开浏览器，直接验证「SAPI 令牌 → CLSID → ISpTTSEngine::Speak」这条链路：

```powershell
build\voicevox-sapi-check.exe list
build\voicevox-sapi-check.exe say "俊达萌" "こんにちは。" demo.wav

# 名字匹配是「任一语言的值里包含这个子串」，中日英随便用哪个
build\voicevox-sapi-check.exe say "Zundamon" "こんにちは。" demo.wav

# 换算可以用数字验：rate 10 应让时长减半，volume 50 应让峰值减半
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" a.wav 0 100
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" b.wav 10 100
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" c.wav 0 50

# 单进程连读多个角色，验证模型 LRU 淘汰并给出每个角色的延迟
build\voicevox-sapi-check.exe bench .\out 10
```

`bench` 是唯一能测到 LRU 的入口：每次 `say` 都是新进程，缓存为空，淘汰路径走不到。配合
`VOICEVOX_SAPI_MAX_PACKS=1` 可以强制每次换包都卸载。

### check-chrome.mjs

起一个临时 Chrome，读 `speechSynthesis.getVoices()` 并真的朗读一句：

```powershell
node tools\check-chrome.mjs          # 加 --headed 可以看到窗口
```

### 命令行安装器

给开发和高级部署用，可以只装指定模型：

```powershell
voicevox-sapi-setup.exe install --source "C:\path\to\voicevox_core" --models all
```

`--source` 指向解压后的 voicevox_core 发布目录（`c_api\lib`、`onnxruntime\lib`、`dict`、
`models\vvms`）。安装器会复制运行时和词典到 `%ProgramData%\VoicevoxSapi\`，按 `--models` 复制
模型包（`all` = 全部；`0,3,8` = 只装这几个；默认 `keep` 不动已有），复制 DLL 到
`%ProgramFiles%\VoicevoxSapi\`，调用 `DllRegisterServer`，最后枚举 SAPI 确认条数。

```powershell
voicevox-sapi-setup.exe status      # DLL / 运行时 / 模型 / 语音条数
```

`--prune` 让命令行路径也遵守「装什么就留什么」：复制完模型后删掉不在本次 `--models` 列表里的
`.vvm`。不加这个参数就是只增不减。没写 `--models` 时 `--prune` 不生效，会打印
`prune: skipped (needs an explicit --models)`。

```powershell
voicevox-sapi-setup.exe install --models n0 --prune   # 只留 n0.vvm
```

安装器只会删除 `%ProgramFiles%\VoicevoxSapi` 和 `%ProgramData%\VoicevoxSapi` 这两个自己的目录，
递归删除前会校验末级目录名。

`voicevox_core.dll` 是延迟加载的，由 `dllmain.cpp` 的钩子按绝对路径解析：SAPI 在浏览器进程里
加载本 DLL 时，默认依赖搜索顺序看的是浏览器目录，不是本 DLL 的目录。

## 已知限制

- 只支持 64 位宿主，32 位 SAPI 视图不可用（voicevox_core 只发布 x86_64）
- 音高不可调（SAPI 引擎接口没有 pitch 通道）
- 流式是按句的，句内仍整句合成；一个长句没有标点时首字延迟等于整句时长（超过 60 字会在逗号处再切）
- 引擎在宿主进程内，崩溃会带走宿主
- 首次载入一个模型包需要 1~2 秒（`n0.vvm` 约 8 秒），之后按 LRU 常驻最多 4 个
- 安装包未签名，Windows 会显示 SmartScreen 警告；自签名证书对 SmartScreen 没有帮助
- 不提供开始菜单项，也没有自动更新
