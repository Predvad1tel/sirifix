/*
 * SiriFix.c — твик для LiveContainer (iOS, arm64).
 *
 * ЗАЧЕМ
 *   VK при запуске обращается к SiriKit (класс INPreferences). Без права
 *   com.apple.developer.siri (его нельзя получить с бесплатным Apple ID)
 *   система бросает исключение, и приложение падает:
 *     "Use of the class <INPreferences> from an app requires the entitlement
 *      com.apple.developer.siri"
 *
 * ЧТО ДЕЛАЕТ
 *   1. Подгружает системный Intents.framework.
 *   2. В классах INPreferences и INVocabulary заменяет ВСЕ методы
 *      (и классовые +, и экземплярные -) на заглушки, которые ничего не
 *      проверяют и ничего не бросают:
 *        +siriAuthorizationStatus   -> «доступ запрещён» (2)
 *        +requestSiriAuthorization: -> вызывает колбэк со статусом «запрещён»
 *        все остальные              -> возвращают 0 / nil
 *      Служебные методы (alloc, dealloc, description и т.п.) не трогает.
 *   3. Если классов ещё нет, повторяет замену, когда загрузится новая библиотека.
 *
 * ДВЕ СБОРКИ ИЗ ОДНОГО ФАЙЛА
 *   SiriFixQuiet.dylib  — только пункты 1–3, больше ничего. Рабочая версия.
 *   SiriFixReport.dylib — (-DSIRIFIX_REPORT) то же самое, плюс сразу после запуска
 *                         бросает исключение, текст которого — отчёт: что найдено
 *                         и сколько методов заменено. Окно ошибки LiveContainer
 *                         его покажет. VK с этой сборкой НЕ запустится, это
 *                         только проверка.
 *
 * ЧЕГО НЕ ДЕЛАЕТ
 *   Нет сети, нет файлов, нет буфера обмена, нет доступа к паролям и данным VK.
 *
 * ОГРАНИЧЕНИЕ
 *   Это обход ровно одной ошибки. Не проверено, что после неё VK не упадёт
 *   на чём-то другом.
 *
 * Системные заголовки не подключаются намеренно: объявлено только то, что
 * реально используется, чтобы файл можно было прочитать целиком.
 */

typedef struct objc_class    *Class;
typedef struct objc_object   *id;
typedef struct objc_selector *SEL;
typedef struct objc_method   *Method;
typedef void (*IMP)(void);
struct mach_header;

extern Class       objc_getClass(const char *name);
extern Class       object_getClass(id obj);
extern Method     *class_copyMethodList(Class cls, unsigned int *out_count);
extern SEL         method_getName(Method m);
extern const char *sel_getName(SEL sel);
extern IMP         method_setImplementation(Method m, IMP imp);

extern void *dlopen(const char *path, int mode);
extern void  free(void *p);

extern void *_Block_copy(const void *block);
extern void  _Block_release(const void *block);
extern void *dispatch_get_global_queue(long identifier, unsigned long flags);
extern void  dispatch_async_f(void *queue, void *context, void (*work)(void *));
extern void  _dyld_register_func_for_add_image(void (*func)(const struct mach_header *, long));

#ifdef SIRIFIX_REPORT
extern SEL  sel_registerName(const char *name);
extern void objc_msgSend(void);
extern void objc_exception_throw(id exception);
#endif

#define RTLD_NOW 2

/* Раскладка Objective-C блока в памяти (стабильная часть ABI). */
struct Block {
    void *isa;
    int   flags;
    int   reserved;
    void (*invoke)(void *block, long status);
};

/* INSiriAuthorizationStatus: 0 не задан, 1 ограничен, 2 запрещён, 3 разрешён. */
#define SIRI_DENIED 2

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------- заглушки ---------- */

/* По умолчанию: ничего не делает, возвращает 0 / nil / NO. */
static long stub_zero(void *self, void *cmd) {
    (void)self; (void)cmd;
    return 0;
}

/* +[INPreferences siriAuthorizationStatus] */
static long stub_status(void *self, void *cmd) {
    (void)self; (void)cmd;
    return SIRI_DENIED;
}

static void deliver_denied(void *block) {
    ((struct Block *)block)->invoke(block, SIRI_DENIED);
    _Block_release(block);
}

/* +[INPreferences requestSiriAuthorization:] — отвечаем «запрещено» асинхронно,
 * как это делает настоящая реализация. */
static void stub_request(void *self, void *cmd, void *handler) {
    (void)self; (void)cmd;
    if (!handler) return;
    void *copy = _Block_copy(handler);
    dispatch_async_f(dispatch_get_global_queue(0, 0), copy, deliver_denied);
}

/* ---------- отчёт (нужен только для сборки Report) ---------- */

static char     report[3000];
static unsigned report_len;

static void add(const char *s) {
    while (*s && report_len < sizeof(report) - 1) report[report_len++] = *s++;
    report[report_len] = 0;
}

static void add_num(unsigned v) {
    char digits[12];
    int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) {
        char c[2] = { digits[--n], 0 };
        add(c);
    }
}

#ifdef SIRIFIX_REPORT
/* Бросает NSException, чей текст — отчёт. Вызывается из очереди libdispatch,
 * тем же путём, каким до LiveContainer доходит ошибка самого VK. */
static void throw_report(void *unused) {
    (void)unused;
    id ns_string = (id)objc_getClass("NSString");
    id ns_exc    = (id)objc_getClass("NSException");
    if (!ns_string || !ns_exc) return;

    SEL from_utf8 = sel_registerName("stringWithUTF8String:");
    id name   = ((id (*)(id, SEL, const char *))objc_msgSend)(ns_string, from_utf8, "SiriFixReport");
    id reason = ((id (*)(id, SEL, const char *))objc_msgSend)(ns_string, from_utf8, report);
    id exc    = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
        ns_exc, sel_registerName("exceptionWithName:reason:userInfo:"), name, reason, (id)0);
    if (exc) objc_exception_throw(exc);
}
#endif

/* ---------- замена методов ---------- */

static const char *const TARGETS[] = { "INPreferences", "INVocabulary" };
static int done[2];

/* Служебные методы, которые трогать не нужно. */
static const char *const KEEP[] = {
    "load", "initialize", "alloc", "allocWithZone:", "new", "class", "self",
    "description", "debugDescription", "hash", "isEqual:",
    "dealloc", ".cxx_destruct", "retain", "release", "autorelease", "retainCount",
    0
};

static int keep_original(const char *name) {
    for (int i = 0; KEEP[i]; i++)
        if (same(name, KEEP[i])) return 1;
    return 0;
}

/* Заменяет методы одного списка (класс или метакласс). Возвращает, сколько заменено. */
static unsigned patch_list(Class c, int logging) {
    unsigned count = 0, replaced = 0;
    Method *methods = class_copyMethodList(c, &count);
    if (!methods) return 0;

    for (unsigned i = 0; i < count; i++) {
        const char *name = sel_getName(method_getName(methods[i]));
        if (logging) { add(" "); add(name); }
        if (keep_original(name)) continue;

        IMP replacement = (IMP)stub_zero;
        if (same(name, "siriAuthorizationStatus"))        replacement = (IMP)stub_status;
        else if (same(name, "requestSiriAuthorization:")) replacement = (IMP)stub_request;

        method_setImplementation(methods[i], replacement);
        replaced++;
    }
    free(methods);
    return replaced;
}

static void patch_class(int idx, int logging) {
    const char *name = TARGETS[idx];
    Class cls = objc_getClass(name);
    if (!cls) {
        if (logging) { add(name); add(": class NOT FOUND\n"); }
        return;
    }
    if (done[idx]) return;
    done[idx] = 1;

    if (logging) { add(name); add("\n class methods:"); }
    unsigned a = patch_list(object_getClass((id)cls), logging); /* метакласс = методы + */
    if (logging) add("\n instance methods:");
    unsigned b = patch_list(cls, logging);
    if (logging) { add("\n replaced: "); add_num(a); add(" + "); add_num(b); add("\n"); }
}

/* Запасной путь: классов не было, когда твик загрузился. */
static void on_image(const struct mach_header *mh, long slide) {
    (void)mh; (void)slide;
    if (done[0] && done[1]) return;
    patch_class(0, 0);
    patch_class(1, 0);
}

__attribute__((constructor))
static void sirifix_init(void) {
    add("SiriFix report\n");

    void *intents = dlopen("/System/Library/Frameworks/Intents.framework/Intents", RTLD_NOW);
    add("dlopen Intents: ");
    add(intents ? "ok\n" : "FAILED\n");

    patch_class(0, 1);
    patch_class(1, 1);

    if (!(done[0] && done[1])) {
        add("late patch armed\n");
        _dyld_register_func_for_add_image(on_image);
    }

#ifdef SIRIFIX_REPORT
    dispatch_async_f(dispatch_get_global_queue(0, 0), 0, throw_report);
#endif
}
