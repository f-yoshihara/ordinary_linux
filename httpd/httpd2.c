#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>

#define SERVER_NAME "LittleHTTP"
#define SERVER_VERSION "1.0"
#define HTTP_MINOR_VERSION 0
#define BLOCK_BUF_SIZE 1024
#define LINE_BUF_SIZE 4096
#define MAX_REQUEST_BODY_LENGTH (1024 * 1024)

/****** Data Type Definitions ********************************************/

struct HTTPHeaderField
{
    char *name; // header名
    char *value; // 値
    struct HTTPHeaderField *next; // リンクリスト。次のHTTPHeaderFieldのポインタ
};

struct HTTPRequest
{
    int protocol_minor_version; // プロトコルのマイナーバージョン
    char *method; // リクエストメソッド
    char *path; // リクエストのパス
    struct HTTPHeaderField *header; // HTTPヘッダ これは既に定義されている
    char *body; // エンティティボディ
    long length; // エンティティボディのサイズ
};

struct FileInfo
{
    char *path;
    long size;
    int ok;
};


/****** Function Prototypes **********************************************/

static void setup_environment(char *root, char *user, char *group);
typedef void (*sighandler_t)(int);
static void install_signal_handlers(void);
static void trap_signal(int sig, sighandler_t handler);
static void detach_children(void);
static void signal_exit(int sig);
static void noop_handler(int sig);
static void become_daemon(void);
static int listen_socket(char *port);
static void server_main(int server, char *docroot);
static void service(FILE *in, FILE *out, char *docroot);
static struct HTTPRequest* read_request(FILE *in);
static void read_request_line(struct HTTPRequest *req, FILE *in);
static struct HTTPHeaderField* read_header_field(FILE *in);
static void upcase(char *str);
static void free_request(struct HTTPRequest *req);
static long content_length(struct HTTPRequest *req);
static char* lookup_header_field_value(struct HTTPRequest *req, char *name);
static void respond_to(struct HTTPRequest *req, FILE *out, char *docroot);
static void do_file_response(struct HTTPRequest *req, FILE *out, char *docroot);
static void method_not_allowed(struct HTTPRequest *req, FILE *out);
static void not_implemented(struct HTTPRequest *req, FILE *out);
static void not_found(struct HTTPRequest *req, FILE *out);
static void output_common_header_fields(struct HTTPRequest *req, FILE *out, char *status);
static struct FileInfo* get_fileinfo(char *docroot, char *path);
static char* build_fspath(char *docroot, char *path);
static void free_fileinfo(struct FileInfo *info);
static char* guess_content_type(struct FileInfo *info);
static void* xmalloc(size_t sz);
static void log_exit(const char *fmt, ...);

/****** Functions ********************************************************/

#define USAGE "Usage: %s [--port=n] [--chroot --user=u --group=g] [--debug] <docroot>\n"

static int debug_mode = 0;

static struct option longopts[] = {
    {"debug",  no_argument,       &debug_mode, 1},
    {"chroot", no_argument,       NULL, 'c'},
    {"user",   required_argument, NULL, 'u'},
    {"group",  required_argument, NULL, 'g'},
    {"port",   required_argument, NULL, 'p'},
    {"help",   no_argument,       NULL, 'h'},
    {0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
    int server_fd;
    char *port = NULL;
    char *docroot;
    int do_chroot = 0;
    char *user = NULL;
    char *group = NULL;
    int opt;

    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case 0:
            break;
        case 'c':
            do_chroot = 1;
            break;
        case 'u':
            user = optarg;
            break;
        case 'g':
            group = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'h':
            fprintf(stdout, USAGE, argv[0]);
            exit(0);
        case '?':
            fprintf(stderr, USAGE, argv[0]);
            exit(1);
        }
    }
    if (optind != argc - 1) {
        fprintf(stderr, USAGE, argv[0]);
        exit(1);
    }
    docroot = argv[optind];

    if (do_chroot) {
        setup_environment(docroot, user, group);
        docroot = "";
    }
    // シグナルハンドラを設定する
    install_signal_handlers();
    // 接続待機用のソケットを作成する
    server_fd = listen_socket(port);
    if (!debug_mode) {
        // ログ出力時のパラメータを設定する
        openlog(SERVER_NAME, LOG_PID|LOG_NDELAY, LOG_DAEMON);
        // プロセスをデーモン化する
        become_daemon();
    }
    server_main(server_fd, docroot);
    exit(0);
}

static void setup_environment(char *root, char *user, char *group)
{
    struct passwd *pw;
    struct group *gr;

    if (!user || !group) {
        fprintf(stderr, "use both of --user and --group\n");
        exit(1);
    }
    gr = getgrnam(group);
    if (!gr) {
        fprintf(stderr, "no such group: %s\n", group);
        exit(1);
    }
    // initgroups()の前に行うのが無難
    if (setgid(gr->gr_gid) < 0) {
        perror("setgid(2)");
        exit(1);
    }
    // initgroups()はルートユーザでないと実行することができない。
    if (initgroups(user, gr->gr_gid) < 0) {
        perror("initgroups(2)");
        exit(1);
    }
    pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "no such user: %s\n", user);
        exit(1);
    }
    // docrootをシステム全体のrootとみなすようにする。
    // httpサーバからサーバ管理対象以外のファイルを参照されることを防ぐことができる。
    // chroot(2)はスーパーユーザ以外は実行することができない。
    chroot(root);
    // 別のクレデンシャルに変更してスーパーユーザの権限を放棄する.
    if (setuid(pw->pw_uid) < 0) {
        perror("setuid(2)");
        exit(1);
    }
}

/**
 * プロセスをデーモン化する
 * デーモンとは制御端末から切り離されたプロセスのことなのでその通りのことを行う。
 * 
 **/
static void become_daemon(void)
{
    int n;

    // プロセスのディレクトリをルートに移動する
    if (chdir("/") < 0)
        log_exit("chdir(2) failed: %s", strerror(errno));
    // 標準出入力・標準エラー出力を/dev/nullに向ける
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    // プロセスグループリーダーは新しいセッションやグループを作成できないという制約をもつ。
    // なのでsetsid()を成功させるためにはfork()を行なってグループリーダーである可能性を潰しておくのが良い。
    n = fork();
    if (n < 0) log_exit("fork(2) failed: %s", strerror(errno));
    if (n != 0) _exit(0);
    // 新しいセッションを作成し自分がそのセッションのリーダーとなる。また、グループリーダーにもなる。
    // 新しく作成されたセッションは制御端末を持たない。つまりデーモンとなる。
    // セッションはユーザーの端末からのログインからログアウトまでの操作をまとめる役割を持つがsetsid()で作成されたセッションは制御端末を持たない。
    if (setsid() < 0) log_exit("setsid(2) failed: %s", strerror(errno));
}

/**
 * 接続待機用のソケットを作成する
 * 
 **/
static int listen_socket(char *port)
{
    struct addrinfo hints, *res, *ai;
    int err;

    // mallocで確保した領域の値は不定だが、memsetでは第二引数で埋めてくれる
    memset(&hints, 0, sizeof(struct addrinfo));
    // IPv4に対象を限定
    hints.ai_family = AF_INET;
    // パケットではなくストリームを扱う（TCP）ことを宣言
    hints.ai_socktype = SOCK_STREAM;
    // 対象を自分のIPアドレスにしている。つまりクライアントではなく、サーバーであることを意味する。
    hints.ai_flags = AI_PASSIVE;
    // 対象は自分なのでホスト名の指定はNULLにする。
    // hintsに当てはまる(自分自身でIPv4)アドレス構造体のリストを取得
    if ((err = getaddrinfo(NULL, port, &hints, &res)) != 0)
        log_exit(gai_strerror(err));
    for (ai = res; ai; ai = ai->ai_next) {
        int sock;
        // 条件に適合するソケットを作成する
        // sock自体はただのファイルディスクリプタである。ただしsocket()の返り値のfdはストリームに繋がっているわけではない。
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        // ソケットをアドレスに結びつける
        if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
            close(sock);
            continue;
        }
        // カーネルに接続を待っているソケットが第一引数のものであることを伝えている。
        // デバイスドライバから接続情報を受け取るのはカーネルだが、そのままだとどのプロセスに渡すべきなのかわからないため。
        // bindしたアドレスへの接続のリクエストがあったらこのソケットにデータを渡してくださいとソケットに伝える
        // 第二引数は同時に扱うことのできるリクエストの数。この数だけプロセスをforkして処理をこなす。
        // ソケットに目印をつけて同時接続の数を書いておくような感じ
        if (listen(sock, MAX_BACKLOG) < 0) {
            close(sock);
            continue;
        }
        // アドレス構造体のメモリを解放
        freeaddrinfo(res);
        // 作成したソケットを返す
        return sock;
    }
    log_exit("failed to listen socket");
    return -1;  /* NOT REACH */
}


static void server_main(int server_fd, char *docroot)
{
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof addr;
        int sock;
        int pid;

        // 第一引数のソケットにクライアントが接続するのを待機し、受け入れが完了したら接続済ストリームのファイルディスクリプタを返す。
        // 第二引数のアドレス構造体にクライアントのアドレス情報を書き込む
        // ここで新たにソケット（ファイルディスクリプター）が作成される。このソケットは接続元のストリームと繋がっている
        // 第一引数のソケットはsocket(),bind(),listen()の過程を経てきたものだが、これがストリームに接続されることはない。
        // ストリームへの接続を作成するための元のディスクリプターという役割をもつ。
        sock = accept(server_fd, (struct sockaddr*)&addr, &addrlen);
        if (sock < 0) log_exit("accept(2) failed: %s", strerror(errno));
        // 接続ごとに子プロセスを作成する
        pid = fork();
        // fork()が失敗した場合は子プロセスは作成されず親プロセスでの戻り値が-1になる。
        if (pid < 0) exit(3);
        // 子プロセスはここから始まる。
        if (pid == 0) { // 子プロセスのfork()の戻り値は0
            /* 子プロセス内でのみこの中の処理を行う（サービスを提供する） */

            // ストリームを開く
            FILE *inf = fdopen(sock, "r");
            FILE *outf = fdopen(sock, "w");

            // サービスを提供する（HTTPの世界に入る）
            service(inf, outf, docroot);
            // プロセスを終了する
            exit(0);
        }
        // ここは親プロセスの処理
        // ソケットのファイルディスクリプターを閉じて接続を終了する。
        // 親プロセスでは一瞬でclose()して次の接続の待機をする(accept()に戻る)。
        close(sock);
    }
}

static void upcase(char *str)
{
    char *p;

    for (p = str; *p; p++) {
        *p = (char)toupper((int)*p);
    }
}

/**
 * 構造体requestに割り当てられているメモリを解放する
 * 
 **/
static void free_request(struct HTTPRequest *req)
{
    struct HTTPHeaderField *h, *head;

    // アロー演算子は構造体のメンバにアクセスするために使用する
    // headerには次のheaderのポインタが入っている
    head = req->header;
    while (head){
        // 今回使用するheaderをhに格納する
        h = head;
        // 次のheadを格納する。
        // headを先に解放してしまうとnextも参照できなくなってしまうのでこの時点で取っておく必要がある。
        head = head->next;
        free(h->name);
        free(h->value);
        free(h);
    }
    // memberが持っている値と構造体自体はあくまで別の実体（メモリ）を持っているのでそれぞれ個別に解放する必要がある。
    free(req->method);
    free(req->path);
    free(req->body);
    free(req);
}

/**
 * ファイルディスクリプタを受け取りストリームを解析してリクエスト構造体に格納する
 * 
 **/
static struct HTTPRequest* read_request(FILE *in)
{
    struct HTTPRequest *req;
    struct HTTPHeaderField *h;

    req = xmalloc(sizeof(struct HTTPRequest));
    // 確保されているメモリへのポインタとストリームを受け取ってメモリにラインを書き込む。
    read_request_line(req, in);
    req->header = NULL;
    // ファイルディスクリプタを受け取ってヘッダを取得する。ポインタを進める。
    // 一度に一つづつヘッダを読み込む。ヘッダがなくなったらNULLを返す。
    while (h = read_header_field(in)) {
        // 現在のヘッダのnextに前回のヘッダを入れる。
        // 初回はNULL
        // スタックされている
        h->next = req->header;
        req->header = h;
    }
    req->length = content_length(req);
    if (req->length != 0){
        if (req->length > MAX_REQUEST_BODY_LENGTH){
            log_exit("request body too long");
        }
        req->body = xmalloc(req->length);
        if (fread(req->body,req->length,1,in)<1){
            log_exit("failed to read request body");
        }
    }else{
        req->body = NULL;
    }

    return req;
}

/**
 * ファイルディスクリプタinからリクエストラインを読み込んで構造体リクエストに書き込む
 * 
 **/
static void read_request_line(struct HTTPRequest *req, FILE *in)
{
    // バッファのメモリを確保する
    char buf[LINE_BUF_SIZE];
    // 文字列が格納されているポインタ型のpathとpを宣言する。
    char *path, *p;

    // buf に一行づつ読み込む
    if (!fgets(buf,LINE_BUF_SIZE,in)){
        log_exit("no request line");
    }
    // strchrは第一引数の文字列ないで最初に第二引数のパターンが現れた位置へのポインターを返す
    p = strchr(buf, ' ');
    if (!p){
        log_exit("parse error on request line (1): %s", buf);
    }
    *p++ = '\0';
    // bufは配列の識別子である。bufの先頭のアドレスのポインタである。そしてpは同じ配列内のmethodの終端のポインタである。
    // なのでp - bufは配列のアドレスの差分を取っているためそれがつまりmethodのサイズになる。
    req->method = xmalloc(p - buf);
    strcpy(req->method, buf);
    upcase(req->method);

    // pはmethodの終端のアドレスなのでpathの先頭ということになる。
    path = p;
    p = strchr(path, ' ');
    if (!p){
        log_exit("parse error on request line (2): %s", buf);
    }
    *p++ = '\0';
    req->path = xmalloc(p - path);
    strcpy(req->path, path);

    if(strncasecmp(p,"HTTP/1.", strlen("HTTP/1."))!=0){
        log_exit("parse error on request line (3): %s", buf);
    }
    p += strlen("HTTP/1.");
    req->protocol_minor_version = atoi(p);
}

/**
 * ファイルディスクリプタを受け取ってHTTPHeaderField構造体を作成し、そのポインタを返す
 * 
 **/
static struct HTTPHeaderField* read_header_field(FILE *in)
{
    struct HTTPHeaderField *h;
    char buf[LINE_BUF_SIZE];
    char *p;

    if(!fgets(buf, LINE_BUF_SIZE, in)){
        log_exit("faild to read request header field: %s", strerror(errno));
    }
    if((buf[0]=='\n')||(strcmp(buf, "\r\n")==0)){
        return NULL;
    }
    // ヘッダのkeyとvalは:で区切られている
    p = strchr(buf, ':');
    if(!p){
        log_exit("parse error on request header field: %s", buf);
    }
    *p++ = '\0';
    // 構造体は入れ物だがサイズを持っている
    // それぞれの部屋は別途領域を用意する必要がある。
    h = xmalloc(sizeof(struct HTTPHeaderField));
    h->name = xmalloc(p - buf);
    strcpy(h->name, buf);


    p += strspn(p, " \t");
    h->value = xmalloc(strlen(p) + 1);
    strcpy(h->value, p);

    return h;
}

/**
 * リクエストのエンティティボディのサイズを取得する
 * 
 **/
static long content_length(struct HTTPRequest *req)
{
    char *val;
    long len;
    
    val = lookup_header_field_value(req, "Content-Length");
    if (!val) return 0;
    // 型の変換を行う
    len = atol(val);
    if (len < 0) log_exit("negative Content-Length value");
    return len;
}

/**
 * 指定のヘッダフィールドの値を取得する
 * 
 **/
static char* lookup_header_field_value(struct HTTPRequest *req, char *name)
{
    struct HTTPHeaderField *h;

    for (h = req->header; h; h = h->next) {
        if (strcasecmp(h->name, name) == 0)
            return h->value;
    }
    return NULL;
}

/**
 * メソッドに応じたレスポンスを出力する
 * 
 **/
static void respond_to(struct HTTPRequest *req, FILE *out, char *docroot)
{
    if (strcmp(req->method, "GET") == 0)
        do_file_response(req, out, docroot);
    else if (strcmp(req->method, "HEAD") == 0)
        do_file_response(req, out, docroot);
    else if (strcmp(req->method, "POST") == 0)
        method_not_allowed(req, out);
    else
        not_implemented(req, out);
}

/**
 * 構造体requestからリクエスト情報を受け取ってリクエストされたパスのファイルの内容を出力先に書き込む
 * 
 **/
static void do_file_response(struct HTTPRequest *req, FILE *out, char *docroot)
{
    struct FileInfo *info;

    info = get_fileinfo(docroot, req->path);
    if (!info->ok) {
        free_fileinfo(info);
        not_found(req, out);
        return;
    }
    output_common_header_fields(req, out, "200 OK");
    fprintf(out, "Content-Length: %ld\r\n", info->size);
    fprintf(out, "Content-Type: %s\r\n", guess_content_type(info));
    fprintf(out, "\r\n");
    if (strcmp(req->method, "HEAD") != 0) {
        int fd;
        char buf[BLOCK_BUF_SIZE];
        ssize_t n;

        fd = open(info->path, O_RDONLY);
        if (fd < 0)
            log_exit("failed to open %s: %s", info->path, strerror(errno));
        for (;;) {
            n = read(fd, buf, BLOCK_BUF_SIZE);
            if (n < 0)
                log_exit("failed to read %s: %s", info->path, strerror(errno));
            if (n == 0)
                break;
            if (fwrite(buf, 1, n, out) < n)
                log_exit("failed to write to socket");
        }
        close(fd);
    }
    fflush(out);
    free_fileinfo(info);
}

static void method_not_allowed(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "405 Method Not Allowed");
    fprintf(out, "Content-Type: text/html\r\n");
    fprintf(out, "\r\n");
    fprintf(out, "<html>\r\n");
    fprintf(out, "<header>\r\n");
    fprintf(out, "<title>405 Method Not Allowed</title>\r\n");
    fprintf(out, "<header>\r\n");
    fprintf(out, "<body>\r\n");
    fprintf(out, "<p>The request method %s is not allowed</p>\r\n", req->method);
    fprintf(out, "</body>\r\n");
    fprintf(out, "</html>\r\n");
    fflush(out);
}

static void not_implemented(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "501 Not Implemented");
    fprintf(out, "Content-Type: text/html\r\n");
    fprintf(out, "\r\n");
    fprintf(out, "<html>\r\n");
    fprintf(out, "<header>\r\n");
    fprintf(out, "<title>501 Not Implemented</title>\r\n");
    fprintf(out, "<header>\r\n");
    fprintf(out, "<body>\r\n");
    fprintf(out, "<p>The request method %s is not implemented</p>\r\n", req->method);
    fprintf(out, "</body>\r\n");
    fprintf(out, "</html>\r\n");
    fflush(out);
}

static void not_found(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "404 Not Found");
    fprintf(out, "Content-Type: text/html\r\n");
    fprintf(out, "\r\n");
    if (strcmp(req->method, "HEAD") != 0) {
        fprintf(out, "<html>\r\n");
        fprintf(out, "<header><title>Not Found</title><header>\r\n");
        fprintf(out, "<body><p>File not found</p></body>\r\n");
        fprintf(out, "</html>\r\n");
    }
    fflush(out);
}

#define TIME_BUF_SIZE 64

/**
 * 全リクエストに共通のレスポンスヘッダを出力する
 * 
 **/
static void output_common_header_fields(struct HTTPRequest *req, FILE *out, char *status)
{
    time_t t;
    struct tm *tm;
    char buf[TIME_BUF_SIZE];

    t = time(NULL);
    tm = gmtime(&t);
    if (!tm) log_exit("gmtime() failed: %s", strerror(errno));
    strftime(buf, TIME_BUF_SIZE, "%a, %d %b %Y %H:%M:%S GMT", tm);
    fprintf(out, "HTTP/1.%d %s\r\n", HTTP_MINOR_VERSION, status);
    fprintf(out, "Date: %s\r\n", buf);
    fprintf(out, "Server: %s/%s\r\n", SERVER_NAME, SERVER_VERSION);
    fprintf(out, "Connection: close\r\n");
}

/**
 * 構造体FileInfoのポインタを取得する
 * 
 **/
static struct FileInfo* get_fileinfo(char *docroot, char *urlpath)
{
    struct FileInfo *info;
    struct stat st;

    info = xmalloc(sizeof(struct FileInfo));
    info->path = build_fspath(docroot, urlpath);
    info->ok = 0;
    if (lstat(info->path, &st) < 0) return info;
    if (!S_ISREG(st.st_mode)) return info;
    info->ok = 1;
    info->size = st.st_size;
    return info;
}

/**
 * 指定のフルパスを格納したメモリアドレスのポインタを返す
 * 
 **/
static char * build_fspath(char *docroot, char *urlpath)
{
    char *path;

    path = xmalloc(strlen(docroot)+1+strlen(urlpath)+1);
    sprintf(path, "%s/%s", docroot, urlpath);

    return path;
}

/**
 * inから受け取ったストリームの内容を
 * HTTPRequestの構造に格納し、
 * docrootに流して、outのストリームに出力する
 * 
 **/
static void service(FILE *in, FILE *out, char *docroot)
{
    struct HTTPRequest *req;

    // ストリームをリクエストとして受け取り、パースして構造体を取得する。
    req = read_request(in);
    respond_to(req, out, docroot);
    free_request(req);
}

/**
 * signalを捕捉してハンドリングするための処理
 * signalはカーネルや端末からプロセスへ何かを通知するための手段。
 * いくつかの種類がありそれぞれマクロで名前がつけられているが実体はint型の整数である。
 **/

/**
 * signal捕捉を有効化する
 * 
 **/
static void install_signal_handlers(void)
{
    trap_signal(SIGPIPE, signal_exit);
    detach_children();
}

/**
 * signalを受け取って実際にそれをハンドリングする
 * 
 **/
static void trap_signal(int sig, sighandler_t handler)
{
    struct sigaction act;
    
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    if(sigaction(sig, &act, NULL)<0){
        log_exit("sigaction() failed: %s", strerror(errno));
    }
}

/**
 * 子プロセスの停止・終了時の挙動を設定する
 * 
 **/
static void detach_children(void)
{
    struct sigaction act;

    // no operation 何もしない関数をハンドラとして登録する
    act.sa_handler = noop_handler;
    sigemptyset(&act.sa_mask);
    // SA_NOCLDWAITによって子プロセス終了時にwait()を行わないということをカーネルに伝える。
    // これによって親のために子プロセスの終了時ステータスをカーネルが保持し続ける子のゾンビ化を防ぐことができる。
    act.sa_flags = SA_RESTART | SA_NOCLDWAIT;
    // 子プロセスが停止あるいは終了した際に生成される
    if (sigaction(SIGCHLD, &act, NULL) < 0) {
        log_exit("sigaction() failed: %s", strerror(errno));
    }
}

/**
 * no operation 何もしない関数
 * 
 **/
static void noop_handler(int sig)
{
    ;
}

/**
 * signalの種別を受け取ってlogを出力してからexitする
 * 
 **/
static void signal_exit(int sig)
{
    log_exit("exit by signal %d", sig);
}

/**
 * malloc()を安全に呼び出す
 * 
 **/
static void* xmalloc(size_t sz)
{
    void *p;

    p = malloc(sz);
    if (!p){
        // 失敗時はexitするので呼び出す際のエラーチェックが不要
        log_exit("failed to allocate memory.");
    }
    
    return p;
}

static void free_fileinfo(struct FileInfo *info)
{
    // 中身からfree()する
    free(info->path);
    free(info);
}

static char* guess_content_type(struct FileInfo *info)
{
    return "text/plain";   /* FIXME */
}

/**
 * ログを出力しexitする
 * 
 **/
static void log_exit(char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    // vfprintf()はva_listを渡すことのできるfprintf
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/**
 * ログを出力し退場する
 * 
 **/
static void log_exit(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (debug_mode) {
        // vfprintf()はva_listを渡すことのできるfprintf()
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    else {
        // vsyslog()はva_listを渡すことのできるsyslog()
        vsyslog(LOG_ERR, fmt, ap);
    }
    va_end(ap);
    exit(1);
}
