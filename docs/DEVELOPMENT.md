# 开发文档

面向构建、打包、修改本项目的人。使用方法见仓库根目录的 [README](../README.md)。

## 安装布局

| 路径 | 内容 |
|---|---|
| `%ProgramFiles%\VoicevoxSapi\` | `VoicevoxSapi.dll`、`VoicevoxSapiUninstall.exe`、`voicevox-sapi-setup.exe` |
| `%ProgramData%\VoicevoxSapi\` | `voicevox_core.dll`、`voicevox_onnxruntime.dll`、`dict\`、`voices.txt`、`voices.default.txt` |
| `%ProgramData%\VoicevoxSapi\models\` | 模型包 `*.vvm` |
| `%ProgramData%\VoicevoxSapi\logs\voicevox-sapi.log` | 运行日志（超过 1 MB 轮换为 `.log.1`） |

`%ProgramData%\VoicevoxSapi\` 的权限由安装器显式设置：管理员和 SYSTEM 完全控制，普通用户只读。
`%ProgramData%` 默认允许任何用户在子目录里新建文件，而这个目录里的 `voicevox_core.dll` 会被所有朗读的
进程加载，连同它旁边同名的依赖 DLL —— 不收紧的话，普通用户放一个 `VCRUNTIME140_1.dll` 进去，就能在
下一个管理员的浏览器里执行代码。只有 `logs\` 对普通用户可写（浏览器通常以普通用户运行），那里不会
加载任何东西。安装时还会删掉目录里不属于本产品的 `.dll`。

一个安装包同时决定**注册哪些语音**和**机器上保留哪些模型**：`models\` 里不属于这个包的 `.vvm`
会被删除。安装包自带的 `voices.txt` 落在 `voices.default.txt`；只有 `voices.txt` 不存在、或者仍是
上一个安装包放进去的原样，才会被替换。

## 语音名称的注册方式

名字在注册时一次写三份，SAPI 按当前界面语言挑一份显示：

| 注册表值名 | 给谁看 |
|---|---|
| 默认值 | 回退用（同时写进 `Attributes\Name`），按安装时的系统语言 |
| `0411` | 日文系统 |
| `0409` | 英文系统 |
| `0804` | 中文系统 |

名称表在 `src/voices_i18n.cpp`，`docs/voices.html` 由同一份表生成。表里没有的角色退回日文原名。

`voices.txt` 解析规则：UTF-8（带不带 BOM）、UTF-16、ANSI 都能读；只有注释或文件不存在表示不筛选；
`*` 表示全部；一条已安装语音都选不中时忽略整个列表并在日志里写 `voices.txt ignored`。

注册是整批替换，但顺序是**先写后清**：先逐条重建本次要注册的令牌和 CLSID，全部写完后再删掉不在
这一批里的 `VOICEVOX_` 令牌和本产品形状的 CLSID。中途失败时，旧的语音还在，不会只剩前几条。

反注册有两层：先调用 DLL 的 `DllUnregisterServer`，然后不管成败，安装器自己再按同样的特征扫一遍
（`SweepVoiceRegistrations`，在 `tools/install.h`），最后枚举确认两个类别里都没有 VOICEVOX 语音。
所以 DLL 丢失或加载不了时，既不会卸不掉，也不会留下指向空处的语音。CLSID 的形状判断在
`src/catalog.cpp` 和 `tools/install.h` 各有一份，改 CLSID 编码时两处要一起改。

安装器在解压完文件、注册语音**之前**就写入「应用和功能」卸载项（语音数先记为 0，成功后更新），
所以注册失败的安装也能从设置里卸载。

## 技术说明

| 层 | 说明 |
|---|---|
| `VoicevoxSapi.dll` | 进程内 COM 服务器，实现 `ISpTTSEngine` + `ISpObjectWithToken`，被宿主进程加载 |
| 一个角色 = 一个 CLSID | style id 编在 CLSID 里（`Data1` 低字节 + `Data4[3]` 高字节），`DllGetClassObject` 反解，一个 DLL 服务所有角色 |
| 合成 | `voicevox_synthesizer_create_audio_query` → 改 scale → `voicevox_synthesizer_synthesis` → 剥 RIFF 头 → 按 8 KB 分块 `Write` |
| 流式 | 按句末标点切分，逐句合成逐句写出，首字延迟等于第一句的合成时间；只有标点的尾巴（`やった！！` 的第二个 `！`）并入前一句 |
| 事件 | 每句开头发 `SPEI_SENTENCE_BOUNDARY` 和 `SPEI_WORD_BOUNDARY`（VOICEVOX 没有词级时间，词边界覆盖整句），`<bookmark>` 发 `SPEI_TTS_BOOKMARK`；支持按句跳过（`SPVES_SKIP`）和朗读中途改语速/音量 |
| 输出格式 | 24 kHz / 16 bit / 单声道，与 `GetOutputFormat` 声明一致 |
| 内存 | 模型包懒加载，最近使用的 4 个常驻，超出按 LRU 卸载；core 初始化后 DLL 钉在进程里，不会被 COM 卸载后泄漏合成器 |

SAPI 把引擎加载进**调用方进程**，所以引擎崩溃会带走宿主；失败路径都静默返回 `S_OK`（避免某些宿主
弹对话框），错误原因记在日志里。单句失败不影响其余句子。

`voicevox_core.dll` 用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` 加载：它的
依赖只在它自己的目录和 System32 里找，不看宿主目录、当前目录或 PATH。数据目录在 Release 构建里固定
为 `%ProgramData%\VoicevoxSapi`；`VOICEVOX_SAPI_HOME` 只在 Debug 构建里生效 —— 环境变量可以由权限
更低的进程写进 `HKCU\Environment`，不能让它决定提权进程从哪里加载 DLL。

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
| `GetRate()` + `<rate>` | -10..10 / 0 | `speedScale` 1.0 中性 | `10^(rate/10)`，夹到 0.5~2.0 |
| `GetVolume()` × `<volume>` | 0..100 / 100 | `volumeScale` 1.0 中性 | `volume/100 × level/100` |
| `<pitch absmiddle>` | -10..10 / 0 | `pitchScale` **0.0** 中性 | `0.15 × pitch/10` |

语速用 `10^(rate/10)`，是 Chrome 换算的逆运算：Chrome 把网页的 `utterance.rate` 换成
`int(10 × log10(rate))` 交给 SAPI，所以网页 rate 2 到这里是 3，还原回 2 倍速。之前用的 `2^(rate/10)`
会把同一个请求变成 1.23 倍。

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

`build.bat` 自己按这个顺序解析出目录，每次都显式传给 CMake。直接调用 `cmake` 时，目录在第一次
configure 后会被缓存，之后改环境变量不再生效，需要用 `-DVOICEVOX_CORE_DIR=...` 覆盖。

目录里找不到 `c_api\include\voicevox_core.h` 时，CMake 会停下来并把上面两个下载地址打出来。

### 编译

```powershell
.\build.bat
# 产物：build\VoicevoxSapi.dll、voicevox-sapi-setup.exe、voicevox-sapi-check.exe
#       build\VoicevoxSapiSetup.exe（未附加 payload，单独运行会提示不是完整安装包）
#       编译完自动运行 build\voicevox-unit-tests.exe，测试失败则 build.bat 返回错误

.\package.bat            # generic，6 条通用声
.\package.bat classic    # 4 个角色 / 10 条语音
.\package.bat all        # 全部 136 条
.\package.bat generic D:\path\to\voicevox_core   # 换一份 voicevox_core 解压目录
# 产物：dist\VoicevoxSapiSetup.exe
```

**每次运行都有日志。** `build.bat` 写 `build\logs\build-<时间>.log`，`package.bat` 写
`build\logs\package-<时间>.log`，它调用的 `build.bat` 写进同一个文件，一次打包只需要看一份日志。
逻辑在 `tools\buildlog.cmd`，两个脚本共用。日志里有：

- 运行参数、目录、机器、Windows 版本、git 分支 / 提交 / 未提交的改动
- 选中的 Visual Studio、MSVC、SDK、voicevox_core 目录，`PATH` / `INCLUDE` / `LIB`，
  cmake、ninja、cl 的版本，voicevox_core 目录里有什么
- 每一步的完整输出，包括 `cmake --build --verbose` 打出的每一条编译、链接命令
- 打包时：在场的模型包、角色表校验结果、payload spec 全文、安装包大小和 SHA-256

控制台每一步只打一行。失败时打印失败的步骤、退出码、从日志里挑出来的错误行（编译 / 链接 / CMake 错误、
ninja 的 `FAILED`、各工具的 `cannot` / `not found` 等），以及日志路径，例如：

```
[build] FAILED at step "compile and link (ninja)" (exit code 1)
[build] error lines from the log:
    FAILED: CMakeFiles/voicevox-unit-tests.dir/tests/unit_tests.cpp.obj
    ...\tests\unit_tests.cpp(83): error C2065: "undefined_symbol": undeclared identifier
    ninja: build stopped: subcommand failed.
[build] full log: ...\build\logs\build-20261104-032115.log
```

只保留最近 30 份日志。日志在 `build\` 下，不进 git。拿到别人的安装包时，用 `certutil -hashfile
VoicevoxSapiSetup.exe SHA256` 算出哈希，就能在打包日志里找到对应的那一次构建。

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
名字都还能用，但发出来的是另一个角色的声音，全程不报错。所以 `package.bat` 会运行
`voicevox-model-metas --verify`，用磁盘上的 `.vvm` 逐包核对：每个**在场**的模型包，表里属于它的
行必须和它实际产出的行完全一致，对不上就拒绝打包并给出重新生成的命令。不在场的包不参与比较，
所以只下了 `n0.vvm` 也能打 `generic` 包。

**构建可复现。** Release 构建加了 `/Brepro`，同一份源码在同一个目录里连编两次，产物逐字节相同
（`package.bat` 的输出也一样），所以「这次重编到底改没改东西」可以直接用哈希回答。换一个目录
重建时会有几十个字节的差异，那是链接器记录的输入路径哈希，不影响功能。

### 版本号

版本号只写在 `src/res/version.h` 一处，三个宏相邻：

```c
#define VV_VERSION_COMMA   1,1,0,0     // VERSIONINFO 的数字形式
#define VV_VERSION_STRING  "1.1.0.0"   // 文件属性里的字符串
#define VV_VERSION_DISPLAY "1.1.0"     // 「应用和功能」里的 DisplayVersion
```

`src/res/version_info.inc`（四个二进制共用的 VERSIONINFO）和 `tools/install.h`（卸载项的
`DisplayVersion`）都引用这几个宏。`.rc` 文件对 `version.h`、`version_info.inc`、`app.ico` 的依赖
在 CMake 里显式声明了（rc.exe 的 `#include` 不会被自动扫描），所以改完增量构建就会生效。改完要
重新跑 `package.bat`，否则 `dist/` 里还是旧版本。

**模型版本不记录在产物里。** 安装包带哪一版 `voicevox_core` 和 `.vvm`，完全由 `package.bat` 的
core 目录决定，VERSIONINFO 和日志里都没有。当前这一版对应 core 0.17.1 + vvm 0.17.0，
见「先准备外部文件」。所以只看一个装好的包，是查不出它带的是哪版模型的。

### 单文件安装包

`tools/pack-payload.cpp` 把运行时、词典、模型直接追加在 exe 的 PE 镜像后面，尾部写一个 `VVSPAYL2`
页脚指向文件表，格式定义在 `tools/payload.h`，打包器和安装器共用。不压缩：ONNX 模型和 `sys.dic`
基本压不动，而压缩流里坏一个字节会导致整个安装失败，而不是单个文件复制失败。

每个文件带 FNV-1a 64 校验和，解出来不对就报「安装包已损坏」，不会装上一个坏模型。文件表在使用前
先做边界检查（偏移、长度、路径不得是绝对路径或带 `..`）。安装器通过 PE 安全目录定位页脚，所以对
打好的安装包做 Authenticode 签名（签名追加在文件末尾）不会破坏它。spec 文件里的相对路径相对于 spec
文件所在目录，与从哪个目录运行无关。

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
`--verify src/catalog_table.inc` 逐包核对表和磁盘上的模型包，`package.bat` 打包前就是这样检查的。
名字里出现 `"` 或 `\` 时 `--cpp` 会直接报错，而不是生成一份编不过的表。

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
src/core.cpp             VOICEVOX C API 封装（单锁串行，懒加载，失败重试，模型 LRU）
src/engine.cpp           ISpTTSEngine：GetOutputFormat / Speak（XML 状态、事件、跳过）；ISpObjectWithToken
src/sentences.cpp        分句（带原文偏移，供边界事件使用）
src/audio_util.cpp       AudioQuery JSON 修补、WAV 解析、SAPI → VOICEVOX 参数换算
src/selection.cpp        voices.txt 解析
src/registry.cpp         CLSID 与 SAPI 语音令牌的注册/反注册（按特征清理旧注册）
src/dllmain.cpp          DllGetClassObject / DllRegisterServer / delay-load 钩子
src/log.cpp              写 %ProgramData%\VoicevoxSapi\logs\voicevox-sapi.log（1 MB 轮换）
src/res/                 图标 + VERSIONINFO；version.h 是版本号的唯一来源
tests/unit_tests.cpp     voicevox-unit-tests：上面几个纯函数模块的单元测试，build.bat 自动运行
tools/install.h          两个安装器共用的复制/删除/权限/注册/卸载逻辑
tools/payload.h          单文件安装包的 payload 格式（打包器和安装器共用）
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
tools/buildlog.cmd       build.bat / package.bat 共用的日志与失败汇总
docs/voices.html         136 条语音的中日英对照表（生成的）
docs/DEVELOPMENT.md      本文档
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

# 换算可以用数字验：rate 3（网页 rate 2）应让时长约减半，volume 50 应让峰值减半
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" a.wav 0 100
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" b.wav 3 100
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "こんにちは。" c.wav 0 50

# 音高走 SAPI XML，和 Chrome 传 utterance.pitch 的方式一样（在 cmd 里运行，PowerShell 会吃掉引号）
build\voicevox-sapi-check.exe say "ずんだもん / ノーマル" "<pitch absmiddle='10'>こんにちは。</pitch>" d.wav

# 单进程连读多个角色，验证模型 LRU 淘汰并给出每个角色的延迟
build\voicevox-sapi-check.exe bench .\out 10
```

`bench` 是唯一能测到 LRU 的入口：每次 `say` 都是新进程，缓存为空，淘汰路径走不到。配合
`VOICEVOX_SAPI_MAX_PACKS=1` 可以强制每次换包都卸载。

### check-chrome.mjs

起一个临时 Chrome，读 `speechSynthesis.getVoices()`，用非默认的音高和语速真的朗读一句，并检查
`onboundary` 事件能触发（引擎实际收到的参数看日志里的 `speak:` 行）：

```powershell
node tools\check-chrome.mjs          # 加 --headed 可以看到窗口
```

全部通过时退出码为 0，没有日语语音、朗读出错或超时时为 1，可以直接用在脚本里。出错时也会关掉
Chrome、删掉临时用户目录。

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

`--dest <dir>` 可以把程序目录装到别处（末级目录名仍须是 `VoicevoxSapi`），卸载时会从注册表里
记录的位置删除。数据目录不能改：引擎在宿主进程里只认 `%ProgramData%\VoicevoxSapi`，早先的
`--data` 选项只复制文件、引擎却不去那里读，已经删除。`uninstall` 接受 `--quiet`，供「应用和功能」
的静默卸载命令使用。

`voicevox_core.dll` 是延迟加载的，由 `dllmain.cpp` 的钩子按绝对路径解析：SAPI 在浏览器进程里
加载本 DLL 时，默认依赖搜索顺序看的是浏览器目录，不是本 DLL 的目录。

### 提权程序的 DLL 搜索

`VoicevoxSapiSetup.exe`、`VoicevoxSapiUninstall.exe`、`voicevox-sapi-setup.exe` 都以管理员身份运行，
而且通常放在「下载」这类谁都能往里放文件的目录。三层防护：

- **静态 CRT**（`MSVC_RUNTIME_LIBRARY MultiThreaded`）：去掉对 `VCRUNTIME140.dll` 等非系统 DLL 的导入，
  这些导入在 `main` 之前就会从程序所在目录解析
- **`/DEPENDENTLOADFLAG:0x800`**：系统 DLL 自己的依赖只在 System32 里找
- **`HardenDllSearch()`**：`main` 第一行调用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`，
  之后的运行时加载也只看 System32

卸载程序位于要删除的目录里，需要把自己复制到别处再运行。副本不放 `%TEMP%`（同一用户的非提权进程
可写），而是放在 `%ProgramData%\VoicevoxSapi-uninstall-{GUID}`：目录名不可预测，并且用一次
`CreateDirectoryW` 带着「仅管理员」ACL 创建，没有可以抢先放文件的时间窗口。副本运行后把自己和目录
登记为开机删除。

## 技术限制

- 只支持 64 位宿主，32 位 SAPI 视图不可用（voicevox_core 只发布 x86_64）
- 音高只能在 ±0.15 的 `pitchScale` 范围内调（再大声码器会失真）
- 没有词级时间信息：词边界事件按句发出，跟读高亮只能精确到句
- 流式是按句的，句内仍整句合成；一个长句没有标点时首字延迟等于整句时长（超过 60 字会在逗号处再切）
- 引擎在宿主进程内，崩溃会带走宿主
- 首次载入一个模型包需要 1~2 秒（`n0.vvm` 约 8 秒），之后按 LRU 常驻最多 4 个
- 安装包未签名，Windows 会显示 SmartScreen 警告；自签名证书对 SmartScreen 没有帮助。payload 格式
  按设计兼容 Authenticode（安装器经 PE 安全目录找页脚），但还没有用真实证书签名验证过
- 不提供开始菜单项，也没有自动更新
