# تشغيل المشروع على لوحة 01Studio CanMV K230 (2GB)

هذا الدليل يكمل ما أنجزته المرحلة 0 على الـPC. كل خطوة فيها أمر تحقق واضح؛ لا
تنتقل للخطوة التالية قبل نجاح التحقق. المواضع التي تحتاج كتابة كود على اللوحة
معلَّمة بـ `TODO(k230)` داخل المصادر نفسها.

## 0. ما الذي يعمل الآن (بدون لوحة)

```bash
cmake -S . -B build -G Ninja && ninja -C build && ctest --test-dir build
./build/tools/k230-make-test-recording demo.k230rec 10
./build/inspector/k230-inspector --replay demo.k230rec --realtime --verbose
```

المفكّك (demuxer)، المزامنة بالـPTS الحقيقي، السياسة، وتنسيق الحزم بين
النواتين — كلها نهائية ومختبرة. ما يتبقى هو استبدال الأجزاء الخاصة بالعتاد.

## 1. النواة الصغيرة (Linux) — `k230-bridge`

### 1.1 بناء SDK مع adb

1. استنسخ `k230_sdk` (kendryte) واستخدم حاوية Docker الرسمية.
2. `make CONF=k230_canmv_defconfig prepare_sourcecode` ثم `make`.
3. فعّل في buildroot: `BR2_PACKAGE_ANDROID_TOOLS=y` و `BR2_PACKAGE_ANDROID_TOOLS_ADB=y`
   (`make buildroot-menuconfig` → Target packages → Development tools → android-tools).
4. أعد البناء واكتب الصورة على microSD.

**تحقق:** على اللوحة `adb version` يطبع رقم الإصدار.

### 1.2 USB Host والهاتف

- وصّل الهاتف بمنفذ **USB‑A (Host)** على اللوحة، وليس Type‑C الخاص بالطاقة/التصحيح.
- فعّل USB debugging على الهاتف.

**تحقق:** `lsusb` يعرض الهاتف، ثم `adb devices` يعرض `device`. أول مرة ستظهر
`unauthorized` حتى تقبل مفتاح RSA على الهاتف؛ المفتاح يُحفظ في
`~/.android/adbkey` على اللوحة — انسخه إلى مكان دائم على الـSD.

### 1.3 بناء `k230-bridge` للنواة الصغيرة

```bash
cmake -S . -B build-little -G Ninja \
  -DK230_TARGET=little \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake   # أنشئه من toolchain الـSDK
ninja -C build-little
```

ملف الـtoolchain يحتاج فقط `CMAKE_SYSTEM_NAME=Linux`، `CMAKE_SYSTEM_PROCESSOR=riscv64`
و `CMAKE_C/CXX_COMPILER` من `toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu`.

**تحقق (بدون النواة الكبيرة):**
```bash
./k230-bridge --record /sdcard/capture.k230rec --duration 20
```
ثم انسخ الملف إلى الـPC وشغّله بـ `k230-inspector --replay`. إن رأيت الأحكام
على الـPC فالنواة الصغيرة + adb + scrcpy + المفكّك تعمل بالكامل.

### 1.4 DATAFIFO / IPCMSG (جانب Linux)

الملف: `common/src/ipc/datafifo_channel.cpp`، الدوال معلّمة `TODO(k230)`.
المرجع في SDK: `src/common/cdk/user/samples/sample_datafifo` و
`sample_ipcmsg`. الحزمة المُرسلة هي بالضبط ما تنتجه `ipc::encode_packet`
(رأس 24 بايت + الحمولة)؛ أبقِ حجم عنصر الـDATAFIFO ≥ أكبر حزمة H.264 متوقعة
(`ScrcpyDemuxer::Options::max_packet_size`، افتراضياً 16 MiB — خفّضه إلى 1 MiB
مع `max-size 800`).

## 2. النواة الكبيرة (RT‑Smart) — `k230-inspector`

### 2.1 البناء

```bash
cmake -S . -B build-big -G Ninja -DK230_TARGET=big \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-rtsmart.cmake
```

يُبنى كتطبيق RT‑Smart (elf) يوضع في `/sharefs` ويُشغَّل من `init.sh`
كما في `src/big/mpp/userapps/sample`.

### 2.2 VDEC

الملف: `inspector/src/vdec_decoder.cpp`. المرجع: `sample_vdec` في
`src/big/mpp/userapps/sample/sample_vdec`. الخطوات:

1. `kd_mpi_vdec_create_chn` بـ `K_PT_H264` (أو H265 حسب `CodecId`)، حجم الإطار
   من حزمة الـsession (`width`/`height`).
2. أرسل حزم الـconfig (SPS/PPS) أولاً ثم كل حزمة مضغوطة عبر `kd_mpi_vdec_send_stream`
   مع `pts = MediaPacket::pts_us`.
3. `kd_mpi_vdec_get_frame` يعطي NV12 مع نفس الـPTS → حوّله إلى `VideoFrame`
   (المزامنة تعتمد على هذا الـPTS، لا تستبدله بوقت النظام).

**تحقق:** `--dump-dir /sharefs/dump --dump-every 10` ثم افتح ملفات PPM على الـPC.

### 2.3 KPU

الملف: `inspector/src/kpu_analyzer.cpp`. حوّل نموذج التصنيف (مثلاً
NSFW MobileNet) بـ nncase إلى `.kmodel` مع كوانتزة uint8، وحمّله بـ
`nncase::runtime::interpreter`. المدخل NV12 من VDEC → RGB 224×224.
عرّف `K230_HAS_NNCASE` في CMake عند توفر الـruntime. حتى ذلك الوقت يعمل
`HeuristicAnalyzer` تلقائياً.

## 3. الاختبار الآمن للسياسة

ابدأ دائماً بـ `--warn 0.6 --block 0.85 --confirm 3` وبدون تطبيق مرافق:
الأحكام تُطبع فقط (`k230-inspector --verdicts stdout`). لا تفعّل `delete`
قبل الانتهاء من عقد التطبيق المرافق (README → Companion app contract).

## 4. قائمة تحقق سريعة

| الخطوة | أمر التحقق | النتيجة المتوقعة |
|---|---|---|
| adb على اللوحة | `adb devices` | `XXXX device` |
| scrcpy + demux | `k230-bridge --record` ثم replay على PC | إطارات وأحكام |
| DATAFIFO | `k230-bridge` + `k230-inspector --source datafifo` | `packets=` يتزايد في السجل |
| VDEC | `--dump-dir` | PPM صحيحة |
| المزامنة | السجل `sync: incomplete=` | قريب من 0 |
| KPU | `--kmodel x.kmodel` | `analyzer=kpu` في السجل |
