#include <curl/curl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h> // memmove
#include <time.h>   // strptime, strptime, timegm, localtime
#include <ctype.h>  // isspace
#include <locale.h> // setlocale
#include <curses.h>
#include <ncurses.h>
#include <pthread.h>
#include "config.h"
#include "messages.h"

#define SJSON_IMPLEMENT
#include "sjson.h"

char *streaming_json = NULL;

#define URI_STREAM "api/v1/streaming/"
#define URI_TIMELINE "api/v1/timelines/"

char *selected_stream = "user";
char *selected_timeline = "home";

#define CURL_USERAGENT "curl/" LIBCURL_VERSION

// ストリーミングを受信する関数のポインタ
void (*streaming_received_handler)(void);

// 受信したストリーミングを処理する関数のポインタ
void (*stream_event_handler)(struct sjson_node *);

// インスタンスにクライアントを登録する
void do_create_client(char *, char *);

// Timelineの受信
void get_timeline(void);

// 承認コードを使ったOAuth処理
void do_oauth(char *code, char *ck, char *cs);

// Tootを行う
void do_toot(char *);

// ストリーミングでのToot受信処理,stream_event_handlerへ代入
void stream_event_update(struct sjson_node *);

// ストリーミングでの通知受信処理,stream_event_handlerへ代入
void stream_event_notify(struct sjson_node *);

// タイムラインWindow
WINDOW *scr;

// 投稿欄Window
WINDOW *pad;

// アクセストークン文字列
char access_token[256];

// ドメイン文字列
char domain_string[256];

// コンフィグファイルパス構造体
struct nanotodon_config config;

int term_w, term_h;
int pad_x = 0, pad_y = 0;
int monoflag = 0;
int hidlckflag = 1;
int noemojiflag = 0;

// Unicode文字列の幅を返す(半角文字=1)
int ustrwidth(const char *str)
{
	int size, width, strwidth;

	strwidth = 0;
	while (*str != '\0') {
		uint8_t c;
		c = (uint8_t)*str;
		if (c >= 0x00 && c <= 0x7f) {
			size  = 1;
			width = 1;
		} else if (c >= 0xc2 && c <= 0xdf) {
			size  = 2;
			width = 2;
		} else if (c == 0xef) {
			uint16_t p;
			p = ((uint8_t)str[1] << 8) | (uint8_t)str[2];
			size  = 3;
			if (p >= 0xbda1 && p <= 0xbe9c) {
				/* Halfwidth CJK punctuation */
				/* Halfwidth Katakana variants */
				/* Halfwidth Hangul variants */
				width = 1;
			} else if (p >= 0xbfa8 && p <= 0xbfae) {
				/* Halfwidth symbol variants */
				width = 1;
			} else {
				/* other BMP */
				width = 2;
			}
		} else if ((c & 0xf0) == 0xe0) {
			/* other BMP */
			size  = 3;
			width = 2;
		} else if ((c & 0xf8) == 0xf0) {
			/* Emoji etc. */
			size  = 4;
			width = 2;
		} else {
			/* unexpected */
			size  = 1;
			width = 1;
		}
		strwidth += width;
		str += size;
	}
	return strwidth;
}

// curlのエラーを表示
void curl_fatal(CURLcode ret, const char *errbuf)
{
	size_t len = strlen(errbuf);
	endwin();
	fprintf(stderr, "\n");
	if(len>0) {
		fprintf(stderr, "%s%s", errbuf, errbuf[len-1]!='\n' ? "\n" : "");
	}else{
		fprintf(stderr, "%s\n", curl_easy_strerror(ret));
	}
	exit(EXIT_FAILURE);
} 

// domain_stringとapiエンドポイントを合成してURLを生成する
char *create_uri_string(char *api)
{
	char *s = malloc(256);
	sprintf(s, "https://%s/%s", domain_string, api);
	return s;
}

// jsonツリーをパス形式(ex. "account/display_name")で掘ってjson_objectを取り出す
int read_json_fom_path(struct sjson_node *obj, char *path, struct sjson_node **dst)
{
	char *dup = strdup(path);	// strtokは破壊するので複製
	struct sjson_node *dir = obj;
	int exist = 1;
	char *next_key;
	char last_key[256];
	
	char *tok = dup;
	
	// 現在地ノードが存在する限りループ
	while(exist) {
		// 次のノード名を取り出す
		next_key = strtok(tok, "/");
		tok = NULL;
		
		// パスの終端(=目的のオブジェクトに到達している)ならループを抜ける
		if(!next_key) break;
		strcpy(last_key, next_key);
		
		// 次のノードを取得する

		struct sjson_node *next = sjson_find_member(dir, next_key);

		exist = next != 0 ? 1 : 0;

		if(exist) {
			// 存在しているので現在地ノードを更新
			dir = next;
		}
	}
	
	// strtok用バッファ解放
	free(dup);
	
	// 現在地を結果ポインタに代入
	*dst = dir;
	
	// 見つかったかどうかを返却
	return exist;
}

// curlから呼び出されるストリーミング受信関数
size_t streaming_callback(void* ptr, size_t size, size_t nmemb, void* data) {
	if (size * nmemb == 0)
		return 0;
	
	char **json = ((char **)data);
	
	size_t realsize = size * nmemb;
	
	size_t length = realsize + 1;
	char *str = *json;
	str = realloc(str, (str ? strlen(str) : 0) + length);
	if(*((char **)data) == NULL) strcpy(str, "");
	
	*json = str;
	
	if (str != NULL) {
		strncat(str, ptr, realsize);
		// 改行が来たらデータ終端(一回の受信に収まるとは限らない)
		if(str[strlen(str)-1] == 0x0a) {
			if(*str == ':') {
				// ':'だけは接続維持用
				free(str);
				*json = NULL;
			} else {
				streaming_received_handler();
			}
		}
	}

	return realsize;
}

// ストリーミングでの通知受信処理,stream_event_handlerへ代入
void stream_event_notify(struct sjson_node *jobj_from_string)
{
	struct sjson_node *notify_type, *screen_name, *display_name, *status;
	const char *dname;
	if(!jobj_from_string) return;
	read_json_fom_path(jobj_from_string, "type", &notify_type);
	read_json_fom_path(jobj_from_string, "account/acct", &screen_name);
	read_json_fom_path(jobj_from_string, "account/display_name", &display_name);
	int exist_status = read_json_fom_path(jobj_from_string, "status", &status);
	
	putchar('\a');
	
	// 通知種別を表示に流用するので先頭を大文字化
	char *t = strdup(notify_type->string_);
	t[0] = toupper(t[0]);
	
	// 通知種別と誰からか[ screen_name(display_name) ]を表示
	wattron(scr, COLOR_PAIR(4));
	if(!noemojiflag) waddstr(scr, strcmp(t, "Follow") == 0 ? "👥" : strcmp(t, "Favourite") == 0 ? "💕" : strcmp(t, "Reblog") == 0 ? "🔃" : strcmp(t, "Mention") == 0 ? "🗨" : "");
	waddstr(scr, t);
	free(t);
	waddstr(scr, " from ");
	waddstr(scr, screen_name->string_);
	
	dname = display_name->string_;
	
	// dname(display_name)が空の場合は括弧を表示しない
	if (dname[0] != '\0') {
		wprintw(scr, " (%s)", dname);
	}
	waddstr(scr, "\n");
	wattroff(scr, COLOR_PAIR(4));
	
	sjson_tag type;
	
	type = status->tag;
	
	// 通知対象のTootを表示,Follow通知だとtypeがNULLになる
	if(type != SJSON_NULL && exist_status) {
		stream_event_update(status);
	}
	
	waddstr(scr, "\n");
	wrefresh(scr);
	
	wmove(pad, pad_x, pad_y);
	wrefresh(pad);
}

// ストリーミングでのToot受信処理,stream_event_handlerへ代入
#define DATEBUFLEN	40
void stream_event_update(struct sjson_node *jobj_from_string)
{
	struct sjson_node *content, *screen_name, *display_name, *reblog, *visibility;
	const char *sname, *dname, *vstr;
	struct sjson_node *created_at;
	struct tm tm;
	time_t time;
	char datebuf[DATEBUFLEN];
	int x, y, date_w;
	if(!jobj_from_string) return;
	read_json_fom_path(jobj_from_string, "content", &content);
	read_json_fom_path(jobj_from_string, "account/acct", &screen_name);
	read_json_fom_path(jobj_from_string, "account/display_name", &display_name);
	read_json_fom_path(jobj_from_string, "reblog", &reblog);
	read_json_fom_path(jobj_from_string, "created_at", &created_at);
	read_json_fom_path(jobj_from_string, "visibility", &visibility);
	memset(&tm, 0, sizeof(tm));
	strptime(created_at->string_, "%Y-%m-%dT%H:%M:%S", &tm);
	time = timegm(&tm);
	strftime(datebuf, sizeof(datebuf), "%x(%a) %X", localtime(&time));
	
	vstr = visibility->string_;
	
	if(hidlckflag) {
		if(!strcmp(vstr, "private") || !strcmp(vstr, "direct")) {
			return;
		}
	}
	
	sjson_tag type;
	
	type = reblog->tag;
	sname = screen_name->string_;
	dname = display_name->string_;
	
	// ブーストで回ってきた場合はその旨を表示
	if(type != SJSON_NULL) {
		wattron(scr, COLOR_PAIR(3));
		if(!noemojiflag) waddstr(scr, "🔃 ");
		waddstr(scr, "Reblog by ");
		waddstr(scr, sname);
		// dname(表示名)が空の場合は括弧を表示しない
		if (dname[0] != '\0') wprintw(scr, " (%s)", dname);
		waddstr(scr, "\n");
		wattroff(scr, COLOR_PAIR(3));
		stream_event_update(reblog);
		return;
	}
	
	// 誰からか[ screen_name(display_name) ]を表示
	wattron(scr, COLOR_PAIR(1)|A_BOLD);
	waddstr(scr, sname);
	wattroff(scr, COLOR_PAIR(1)|A_BOLD);
	
	// dname(表示名)が空の場合は括弧を表示しない
	if (dname[0] != '\0') {
		wattron(scr, COLOR_PAIR(2));
		wprintw(scr, " (%s)", dname);
		wattroff(scr, COLOR_PAIR(2));
	}
	
	if(strcmp(vstr, "public")) {
		int vtyp = strcmp(vstr, "unlisted");
		wattron(scr, COLOR_PAIR(3)|A_BOLD);
		waddstr(scr, " ");
		if(noemojiflag) {
			if(!strcmp(vstr, "unlisted")) {
				waddstr(scr, "<UNLIST>");
			} else if(!strcmp(vstr, "private")) {
				waddstr(scr, "<PRIVATE>");
			} else {
				waddstr(scr, "<!DIRECT!>");
			}
		} else {
			if(!strcmp(vstr, "unlisted")) {
				waddstr(scr, "🔓");
			} else if(!strcmp(vstr, "private")) {
				waddstr(scr, "🔒");
			} else {
				waddstr(scr, "✉");
			}
		}
		wattroff(scr, COLOR_PAIR(3)|A_BOLD);
	}
	
	// 日付表示
	date_w = ustrwidth(datebuf) + 1;
	getyx(scr, y, x);
	if (x < term_w - date_w) {
		for(int i = 0; i < term_w - x - date_w; i++) waddstr(scr, " ");
	} else {
		for(int i = 0; i < x - (term_w - date_w); i++) waddstr(scr, "\b");
		waddstr(scr, "\b ");
	}
	wattron(scr, COLOR_PAIR(5));
	waddstr(scr, datebuf);
	wattroff(scr, COLOR_PAIR(5));
	waddstr(scr, "\n");
	
	const char *src = content->string_;
	
	/*waddstr(scr, src);
	waddstr(scr, "\n");*/
	
	// タグ消去処理、2個目以降のの<p>は改行に
	int ltgt = 0;
	int pcount = 0;
	while(*src) {
		// タグならタグフラグを立てる
		if(*src == '<') ltgt = 1;
		
		if(ltgt && strncmp(src, "<br", 3) == 0) waddch(scr, '\n');
		if(ltgt && strncmp(src, "<p", 2) == 0) {
			pcount++;
			if(pcount >= 2) {
				waddstr(scr, "\n\n");
			}
		}
		
		// タグフラグが立っていない(=通常文字)とき
		if(!ltgt) {
			// 文字実体参照の処理
			if(*src == '&') {
				if(strncmp(src, "&amp;", 5) == 0) {
					waddch(scr, '&');
					src += 4;
				}
				else if(strncmp(src, "&lt;", 4) == 0) {
					waddch(scr, '<');
					src += 3;
				}
				else if(strncmp(src, "&gt;", 4) == 0) {
					waddch(scr, '>');
					src += 3;
				}
				else if(strncmp(src, "&quot;", 6) == 0) {
					waddch(scr, '\"');
					src += 5;
				}
				else if(strncmp(src, "&apos;", 6) == 0) {
					waddch(scr, '\'');
					src += 5;
				}
				else if(strncmp(src, "&#39;", 5) == 0) {
					waddch(scr, '\'');
					src += 4;
				}
			} else {
				// 通常文字
				waddch(scr, *((unsigned char *)src));
			}
		}
		if(*src == '>') ltgt = 0;
		src++;
	}
	
	waddstr(scr, "\n");
	
	// 添付メディアのURL表示
	struct sjson_node *media_attachments;
	
	read_json_fom_path(jobj_from_string, "media_attachments", &media_attachments);
	
	if(media_attachments->tag == SJSON_ARRAY) {
		for (int i = 0; i < sjson_child_count(media_attachments); ++i) {
			struct sjson_node *obj = sjson_find_element(media_attachments, i);
			struct sjson_node *url;
			read_json_fom_path(obj, "url", &url);
			if(url->tag == SJSON_STRING) {
				waddstr(scr, noemojiflag ? "<LINK>" : "🔗");
				waddstr(scr, url->string_);
				waddstr(scr, "\n");
			}
		}
	}
	
	// 投稿アプリ名表示
	struct sjson_node *application_name;
	int exist_appname = read_json_fom_path(jobj_from_string, "application/name", &application_name);
	
	// 名前が取れたときのみ表示
	if(exist_appname) {
		type = application_name->tag;
		
		if(type != SJSON_NULL) {
			int l = ustrwidth(application_name->string_);
		
			// 右寄せにするために空白を並べる
			for(int i = 0; i < term_w - (l + 4 + 1); i++) waddstr(scr, " ");
			
			wattron(scr, COLOR_PAIR(1));
			waddstr(scr, "via ");
			wattroff(scr, COLOR_PAIR(1));
			wattron(scr, COLOR_PAIR(2));
			waddstr(scr, application_name->string_);
			waddstr(scr, "\n");
			wattroff(scr, COLOR_PAIR(2));
		}
	}
	
	waddstr(scr, "\n");
	wrefresh(scr);
	
	wmove(pad, pad_x, pad_y);
	wrefresh(pad);
}

// ストリーミングで受信したJSON(接続維持用データを取り除き一体化したもの)
void streaming_received(void)
{
	
	// イベント取得
	if(strncmp(streaming_json, "event", 5) == 0) {
		char *type = strdup(streaming_json + 7);
		if(strncmp(type, "update", 6) == 0) stream_event_handler = stream_event_update;
		else if(strncmp(type, "notification", 12) == 0) stream_event_handler = stream_event_notify;
		else stream_event_handler = NULL;
		
		char *top = type;
		while(*type != '\n') type++;
		type++;
		
		// 後ろにJSONが引っ付いていればJSONバッファへ
		if(*type != 0) {
			free(streaming_json);
			streaming_json = strdup(type);
		}
		free(top);
	}
	
	// JSON受信
	if(strncmp(streaming_json, "data", 4) == 0) {
		if(stream_event_handler) {
			sjson_context* ctx = sjson_create_context(0, 0, NULL);
			struct sjson_node *jobj_from_string = sjson_decode(ctx, streaming_json + 6);
			stream_event_handler(jobj_from_string);
			sjson_destroy_context(ctx);
			stream_event_handler = NULL;
		}
	}
	
	free(streaming_json);
	streaming_json = NULL;
}

// ストリーミング受信スレッド
void *stream_thread_func(void *param)
{
	get_timeline();
	
	CURLcode ret;
	CURL *hnd;
	struct curl_slist *slist1;
	char errbuf[CURL_ERROR_SIZE], *uri;

	slist1 = NULL;
	slist1 = curl_slist_append(slist1, access_token);
	memset(errbuf, 0, sizeof errbuf);
	
	char *uri_stream = malloc(strlen(URI_STREAM) + strlen(selected_stream) + 1);
	
	strcpy(uri_stream, URI_STREAM);
	strcat(uri_stream, selected_stream);

	uri = create_uri_string(uri_stream);

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_URL, uri);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, CURL_USERAGENT);
	curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist1);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(hnd, CURLOPT_TIMEOUT, 0);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *)&streaming_json);
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, streaming_callback);
	curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, errbuf);
	
	streaming_received_handler = streaming_received;
	stream_event_handler = NULL;
	
	ret = curl_easy_perform(hnd);
	if(ret != CURLE_OK) curl_fatal(ret, errbuf);

	curl_easy_cleanup(hnd);
	hnd = NULL;
	free(uri_stream);
	free(uri);
	curl_slist_free_all(slist1);
	slist1 = NULL;
	
	return NULL;
}

// <stb_textedit用宣言>

#define STB_TEXTEDIT_CHARTYPE   wchar_t
#define STB_TEXTEDIT_STRING     text_control

// get the base type
#include "stb_textedit.h"

// define our editor structure
typedef struct
{
	wchar_t *string;
	int stringlen;
} text_control;

// define the functions we need
void layout_func(StbTexteditRow *row, STB_TEXTEDIT_STRING *str, int start_i)
{
	int remaining_chars = str->stringlen - start_i;
	row->num_chars = remaining_chars > 20 ? 20 : remaining_chars; // should do real word wrap here
	row->x0 = 0;
	row->x1 = 20; // need to account for actual size of characters
	row->baseline_y_delta = 1.25;
	row->ymin = -1;
	row->ymax = 0;
}

int delete_chars(STB_TEXTEDIT_STRING *str, int pos, int num)
{
	memmove(&str->string[pos], &str->string[pos + num], (str->stringlen - (pos + num)) * sizeof(wchar_t));
	str->stringlen -= num;
	return 1;
}

int insert_chars(STB_TEXTEDIT_STRING *str, int pos, STB_TEXTEDIT_CHARTYPE *newtext, int num)
{
	str->string = (wchar_t *)realloc(str->string, (str->stringlen + num) * sizeof(wchar_t));
	memmove(&str->string[pos + num], &str->string[pos], (str->stringlen - pos) * sizeof(wchar_t));
	memcpy(&str->string[pos], newtext, (num) * sizeof(wchar_t));
	str->stringlen += num;
	return 1;
}

// define all the #defines needed 

#define STB_TEXTEDIT_STRINGLEN(tc)     ((tc)->stringlen)
#define STB_TEXTEDIT_LAYOUTROW         layout_func
#define STB_TEXTEDIT_GETWIDTH(tc,n,i)  (1) // quick hack for monospaced
#define STB_TEXTEDIT_KEYTOTEXT(key)    (key)
#define STB_TEXTEDIT_GETCHAR(tc,i)     ((tc)->string[i])
#define STB_TEXTEDIT_NEWLINE           '\n'
#define STB_TEXTEDIT_IS_SPACE(ch)      isspace(ch)
#define STB_TEXTEDIT_DELETECHARS       delete_chars
#define STB_TEXTEDIT_INSERTCHARS       insert_chars

#define STB_TEXTEDIT_K_SHIFT           0xffff0100
#define STB_TEXTEDIT_K_CONTROL         0xffff0200
#define STB_TEXTEDIT_K_LEFT            KEY_LEFT
#define STB_TEXTEDIT_K_RIGHT           KEY_RIGHT
#define STB_TEXTEDIT_K_UP              KEY_UP
#define STB_TEXTEDIT_K_DOWN            KEY_DOWN
#define STB_TEXTEDIT_K_LINESTART       KEY_HOME
#define STB_TEXTEDIT_K_LINEEND         KEY_END
#define STB_TEXTEDIT_K_TEXTSTART       KEY_SHOME
#define STB_TEXTEDIT_K_TEXTEND         KEY_SEND
#define STB_TEXTEDIT_K_DELETE          KEY_DC
#define STB_TEXTEDIT_K_BACKSPACE       0x7f
#define STB_TEXTEDIT_K_BACKSPACE_ALT   0x107
#define STB_TEXTEDIT_K_UNDO            KEY_UNDO
#define STB_TEXTEDIT_K_REDO            KEY_REDO
#define STB_TEXTEDIT_K_INSERT          0xffff0400
#define STB_TEXTEDIT_K_WORDLEFT        0xffff0800
#define STB_TEXTEDIT_K_WORDRIGHT       0xffff1000
#define STB_TEXTEDIT_K_PGUP            KEY_PPAGE
#define STB_TEXTEDIT_K_PGDOWN          KEY_NPAGE

#define STB_TEXTEDIT_IMPLEMENTATION
#include "stb_textedit.h"

// </stb_textedit用宣言>

// インスタンスにクライアントを登録する
void do_create_client(char *domain, char *dot_ckcs)
{
	CURLcode ret;
	CURL *hnd;
	struct curl_httppost *post1;
	struct curl_httppost *postend;
	char errbuf[CURL_ERROR_SIZE];
	
	char json_name[256], *uri;
	
	strcpy(json_name, dot_ckcs);
	
	uri = create_uri_string("api/v1/apps");
	
	// クライアントキーファイルをオープン
	FILE *f = fopen(json_name, "wb");

	post1 = NULL;
	postend = NULL;
	memset(errbuf, 0, sizeof errbuf);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "client_name",
				CURLFORM_COPYCONTENTS, "nanotodon",
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "redirect_uris",
				CURLFORM_COPYCONTENTS, "urn:ietf:wg:oauth:2.0:oob",
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "scopes",
				CURLFORM_COPYCONTENTS, "read write follow",
				CURLFORM_END);

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_URL, uri);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(hnd, CURLOPT_HTTPPOST, post1);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, CURL_USERAGENT);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, f);	// データの保存先ファイルポインタを指定
	curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, errbuf);
	
	ret = curl_easy_perform(hnd);
	if(ret != CURLE_OK) curl_fatal(ret, errbuf);
	
	fclose(f);

	curl_easy_cleanup(hnd);
	hnd = NULL;
	free(uri);
	curl_formfree(post1);
	post1 = NULL;
}

// 承認コードを使ったOAuth処理
void do_oauth(char *code, char *ck, char *cs)
{
	char fields[512];
	sprintf(fields, "client_id=%s&client_secret=%s&grant_type=authorization_code&code=%s&scope=read%%20write%%20follow", ck, cs, code);
	
	// トークンファイルをオープン
	FILE *f = fopen(config.dot_token, "wb");
	
	CURLcode ret;
	CURL *hnd;
	struct curl_httppost *post1;
	struct curl_httppost *postend;
	char errbuf[CURL_ERROR_SIZE], *uri;

	post1 = NULL;
	postend = NULL;
	memset(errbuf, 0, sizeof errbuf);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "grant_type",
				CURLFORM_COPYCONTENTS, "authorization_code",
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "redirect_uri",
				CURLFORM_COPYCONTENTS, "urn:ietf:wg:oauth:2.0:oob",
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "client_id",
				CURLFORM_COPYCONTENTS, ck,
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "client_secret",
				CURLFORM_COPYCONTENTS, cs,
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "code",
				CURLFORM_COPYCONTENTS, code,
				CURLFORM_END);

	uri = create_uri_string("oauth/token");

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_URL, uri);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(hnd, CURLOPT_HTTPPOST, post1);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, CURL_USERAGENT);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, f);	// データの保存先ファイルポインタを指定
	curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, errbuf);
	
	ret = curl_easy_perform(hnd);
	if(ret != CURLE_OK) curl_fatal(ret, errbuf);
	
	fclose(f);

	curl_easy_cleanup(hnd);
	hnd = NULL;
	free(uri);
	curl_formfree(post1);
	post1 = NULL;
}

// Tootを行う
void do_toot(char *s)
{
	CURLcode ret;
	CURL *hnd;
	struct curl_httppost *post1;
	struct curl_httppost *postend;
	struct curl_slist *slist1;
	char errbuf[CURL_ERROR_SIZE], *uri;
	
	int is_locked = 0;
	int is_unlisted = 0;
	
	if(*s == '/') {
		if(s[1] != 0) {
			if(s[1] == '/') {
				s++;
			} else if(strncmp(s+1,"private",7) == 0) {
				is_locked = 1;
				s += 1+4;
			} else if(strncmp(s+1,"unlisted",8) == 0) {
				is_unlisted = 1;
				s += 1+8;
			}
		}
	}
	
	FILE *f = fopen("/dev/null", "wb");
	
	uri = create_uri_string("api/v1/statuses");

	post1 = NULL;
	postend = NULL;
	memset(errbuf, 0, sizeof errbuf);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "status",
				CURLFORM_COPYCONTENTS, s,
				CURLFORM_END);
	curl_formadd(&post1, &postend,
				CURLFORM_COPYNAME, "visibility",
				CURLFORM_COPYCONTENTS, is_locked ? "private" : (is_unlisted ? "unlisted" : "public"),
				CURLFORM_END);
	slist1 = NULL;
	slist1 = curl_slist_append(slist1, access_token);

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_URL, uri);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(hnd, CURLOPT_HTTPPOST, post1);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, CURL_USERAGENT);
	curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist1);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, errbuf);

	ret = curl_easy_perform(hnd);
	if(ret != CURLE_OK) curl_fatal(ret, errbuf);

	fclose(f);
	
	curl_easy_cleanup(hnd);
	hnd = NULL;
	free(uri);
	curl_formfree(post1);
	post1 = NULL;
	curl_slist_free_all(slist1);
	slist1 = NULL;
}

// curlから呼び出されるHTL受信関数
size_t htl_callback(void* ptr, size_t size, size_t nmemb, void* data) {
	if (size * nmemb == 0)
		return 0;
	
	char **json = ((char **)data);
	
	size_t realsize = size * nmemb;
	
	size_t length = realsize + 1;
	char *str = *json;
	str = realloc(str, (str ? strlen(str) : 0) + length);
	if(*((char **)data) == NULL) strcpy(str, "");
	
	*json = str;
	
	if (str != NULL) {
		strncat(str, ptr, realsize);
	}

	return realsize;
}

// Timelineの受信
void get_timeline(void)
{
	CURLcode ret;
	CURL *hnd;
	struct curl_slist *slist1;
	char errbuf[CURL_ERROR_SIZE], *uri;

	slist1 = NULL;
	slist1 = curl_slist_append(slist1, access_token);
	memset(errbuf, 0, sizeof errbuf);
	
	char *uri_timeline = malloc(strlen(URI_TIMELINE) + strlen(selected_timeline) + 1);
	
	strcpy(uri_timeline, URI_TIMELINE);
	strcat(uri_timeline, selected_timeline);
	
	uri = create_uri_string(uri_timeline);

	char *json = NULL;

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_URL, uri);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, CURL_USERAGENT);
	curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist1);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *)&json);
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, htl_callback);
	curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, errbuf);
	
	ret = curl_easy_perform(hnd);
	if(ret != CURLE_OK) curl_fatal(ret, errbuf);


	sjson_context* ctx = sjson_create_context(0, 0, NULL);
	struct sjson_node *jobj_from_string = sjson_decode(ctx, json);
	sjson_tag type;
	
	type = jobj_from_string->tag;
	
	if(type == SJSON_ARRAY) {
		for (int i = sjson_child_count(jobj_from_string) - 1; i >= 0; i--) {
			struct sjson_node *obj = sjson_find_element(jobj_from_string, i);
			
			stream_event_update(obj);
		}
	}
	
	sjson_destroy_context(ctx);

	curl_easy_cleanup(hnd);
	hnd = NULL;
	free(uri_timeline);
	free(uri);
	curl_slist_free_all(slist1);
	slist1 = NULL;
}

sjson_node *read_json_from_file(char *path, char **json_p, sjson_context **ctx_p)
{
	char *json;
	FILE *f = fopen(path, "rb");

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	json = malloc(fsize + 1);
	*json_p = json;

	fread(json, fsize, 1, f);
	fclose(f);

	json[fsize] = 0;

	sjson_context* ctx = sjson_create_context(0, 0, NULL);
	*ctx_p = ctx;

	struct sjson_node *jobj_from_string = sjson_decode(ctx, json);

	return jobj_from_string;
}

// メイン関数
int main(int argc, char *argv[])
{
	config.profile_name[0] = 0;
	
	// オプション解析
	for(int i=1;i<argc;i++) {
		if(!strcmp(argv[i],"-mono")) {
			monoflag = 1;
			printf("Monochrome mode.\n");
		} else if(!strcmp(argv[i],"-unlock")) {
			hidlckflag = 0;
			printf("Show DIRECT and PRIVATE.\n");
		} else if(!strcmp(argv[i],"-noemoji")) {
			noemojiflag = 1;
			printf("Hide UI emojis.\n");
		} else if(!strncmp(argv[i],"-profile",8)) {
			i++;
			if(i >= argc) {
				fprintf(stderr,"too few argments\n");
				return -1;
			} else {
				strcpy(config.profile_name,argv[i]);
				printf("Using profile: %s\n", config.profile_name);
			}
		} else if(!strncmp(argv[i],"-timeline",9)) {
			i++;
			if(i >= argc) {
				fprintf(stderr,"too few argments\n");
				return -1;
			} else {
				if(!strcmp(argv[i],"home")) {
					
				} else if(!strcmp(argv[i],"local")) {
					selected_stream = "public/local";
					selected_timeline = "public?local=true";
				} else if(!strcmp(argv[i],"public")) {
					selected_stream = "public";
					selected_timeline = "public?local=false";
				} else {
					fprintf(stderr,"Unknown timeline %s\n", argv[i]);
					return -1;
				}
				
				selected_stream = strdup(argv[i]);
				printf("Using timeline: %s\n", selected_stream);
			}
		} else {
			fprintf(stderr,"Unknown Option %s\n", argv[i]);
		}
	}
	
	nano_config_init(&config);
	
	char *env_lang = getenv("LANG");
	int msg_lang = 0;
	
	if(env_lang && !strcmp(env_lang,"ja_JP.UTF-8")) msg_lang = 1;
	
	// トークンファイルオープン
	FILE *fp = fopen(config.dot_token, "rb");
	if(fp) {
		// 存在すれば読み込む
		fclose(fp);
		struct sjson_context *ctx;
		char *json;
		struct sjson_node *token;
		struct sjson_node *jobj_from_file = read_json_from_file(config.dot_token, &json, &ctx);
		read_json_fom_path(jobj_from_file, "access_token", &token);
		sprintf(access_token, "Authorization: Bearer %s", token->string_);
		FILE *f2 = fopen(config.dot_domain, "rb");
		fscanf(f2, "%255s", domain_string);
		fclose(f2);
		sjson_destroy_context(ctx);
		free(json);
	} else {
		// ない場合は登録処理へ
		char domain[256];
		char *ck;
		char *cs;
		printf(nano_msg_list[msg_lang][NANO_MSG_WELCOME]);
		printf(nano_msg_list[msg_lang][NANO_MSG_WEL_FIRST]);
retry1:
		printf(nano_msg_list[msg_lang][NANO_MSG_INPUT_DOMAIN]);
		printf(">");
		scanf("%255s", domain);
		printf("\n");
		
		// ドメイン名を保存する
		FILE *f2 = fopen(config.dot_domain, "wb");
		fprintf(f2, "%s", domain);
		fclose(f2);

		char dot_ckcs[256];
		if (nano_config_app_token_filename(&config, domain, dot_ckcs, sizeof(dot_ckcs)) >= sizeof(dot_ckcs)) {
			fprintf(stderr, "FATAL: Can't allocate memory. Too long filename.\n");
			exit(EXIT_FAILURE);
		}
		
		char json_name[256];
		strcpy(json_name, dot_ckcs);
		strcpy(domain_string, domain);
		
		// クライアントキーファイルをオープン
		FILE *ckcs = fopen(json_name, "rb");
		if(!ckcs) {
			// なければ作る
			do_create_client(domain, dot_ckcs);
		} else {
			// あったら閉じる
			fclose(ckcs);
		}
		
		// クライアントキーファイルを読む
		struct sjson_context *ctx;
		char *json;
		struct sjson_node *cko, *cso;
		struct sjson_node *jobj_from_file = read_json_from_file(json_name, &json, &ctx);
		int r1 = read_json_fom_path(jobj_from_file, "client_id", &cko);
		int r2 = read_json_fom_path(jobj_from_file, "client_secret", &cso);
		if(!r1 || !r2) {
			// もしおかしければ最初まで戻る
			printf(nano_msg_list[msg_lang][NANO_MSG_SOME_WRONG_DOMAIN]);
			remove(json_name);
			remove(config.dot_domain);
			goto retry1;
		}
		ck = strdup(cko->string_);
		cs = strdup(cso->string_);

		sjson_destroy_context(ctx);
		free(json);
		
		char code[256];
		
		printf(nano_msg_list[msg_lang][NANO_MSG_AUTHCATION]);
		printf(nano_msg_list[msg_lang][NANO_MSG_OAUTH_URL]);
		
		// 認証用URLを表示、コードを入力させる
		printf("https://%s/oauth/authorize?client_id=%s&response_type=code&redirect_uri=urn:ietf:wg:oauth:2.0:oob&scope=read%%20write%%20follow\n", domain, ck);
		printf(">");
		scanf("%255s", code);
		printf("\n");
		
		// 承認コードで認証
		do_oauth(code, ck, cs);
		free(ck);
		free(cs);

		// トークンファイルを読む
		struct sjson_node *token;
		jobj_from_file = read_json_from_file(config.dot_token, &json, &ctx);
		int r3 = read_json_fom_path(jobj_from_file, "access_token", &token);
		if(!r3) {
			// もしおかしければ最初まで戻る
			printf(nano_msg_list[msg_lang][NANO_MSG_SOME_WRONG_OAUTH]);
			remove(json_name);
			remove(config.dot_domain);
			remove(config.dot_token);
			goto retry1;
		}

		sjson_destroy_context(ctx);
		free(json);

		// httpヘッダに添付する用の形式でコピーしておく
		sprintf(access_token, "Authorization: Bearer %s", token->string_);
		printf(nano_msg_list[msg_lang][NANO_MSG_FINISH]);
	}
	
	setlocale(LC_ALL, "");
	
	WINDOW *term = initscr();
	
	start_color();
	
	use_default_colors();

	if(!monoflag) {
		init_pair(1, COLOR_GREEN, -1);
		init_pair(2, COLOR_CYAN, -1);
		init_pair(3, COLOR_YELLOW, -1);
		init_pair(4, COLOR_RED, -1);
		init_pair(5, COLOR_BLUE, -1);
	} else {
		init_pair(1, -1, -1);
		init_pair(2, -1, -1);
		init_pair(3, -1, -1);
		init_pair(4, -1, -1);
		init_pair(5, -1, -1);
	}
	
	getmaxyx(term, term_h, term_w);
	
	// TL用Window
	scr = newwin(term_h - 6, term_w, 6, 0);
	
	// 投稿欄用Window
	pad = newwin(5, term_w, 0, 0);
	
	scrollok(scr, 1);
	
	wrefresh(scr);
	
	pthread_t stream_thread;
	
	// ストリーミングスレッド生成
	pthread_create(&stream_thread, NULL, stream_thread_func, NULL);
	
	STB_TexteditState state;
	text_control txt;

	txt.string = 0;
	txt.stringlen = 0;

	stb_textedit_initialize_state(&state, 0);
	
	keypad(pad, TRUE);
	noecho();
	
	// 投稿欄との境目の線
	attron(COLOR_PAIR(2));
	for(int i = 0; i < term_w; i++) mvaddch(5, i, '-');
	attroff(COLOR_PAIR(2));
	refresh();
	
	/*mvaddch(0, term_w/2, '[');
	attron(COLOR_PAIR(1));
	addstr("toot欄(escで投稿)");
	attroff(COLOR_PAIR(1));
	mvaddch(0, term_w-1, ']');
	mvaddch(0, 0, '[');
	attron(COLOR_PAIR(2));
	addstr("Timeline(");
	addstr(URI);
	addstr(")");
	attroff(COLOR_PAIR(2));
	mvaddch(0, term_w/2-1, ']');
	refresh();
	wmove(pad, 0, 0);*/
	
	while (1)
	{
		wchar_t c;
		wget_wch(pad, &c);
		if(c == KEY_RESIZE) {
			// リサイズ処理
			getmaxyx(term, term_h, term_w);
			
			// 境目の線再描画
			attron(COLOR_PAIR(2));
			for(int i = 0; i < term_w; i++) mvaddch(5, i, '-');
			attroff(COLOR_PAIR(2));
			refresh();
			
			// Windowリサイズ
			werase(scr);
			wresize(scr, term_h - 6, term_w);
			wresize(pad, 5, term_w);
			
			// TL再取得
			get_timeline();
			
			wrefresh(pad);
			wrefresh(scr);
		} else if(c == 0x1b && txt.string) {
			// 投稿処理
			werase(pad);
			wchar_t *text = malloc(sizeof(wchar_t) * (txt.stringlen + 1));
			memcpy(text, txt.string, sizeof(wchar_t) * txt.stringlen);
			text[txt.stringlen] = 0;
			char status[1024];
			wcstombs(status, text, 1024);
			do_toot(status);
			free(text);
			txt.string = 0;
			txt.stringlen = 0;
		} else {
			// 通常文字
			stb_textedit_key(&txt, &state, c);
		}
		
		// 投稿欄内容表示
		werase(pad);
		wmove(pad, 0, 0);
		int cx=-1, cy=-1;
		for(int i = 0; i < txt.stringlen; i++) {
			if(i == state.cursor) getyx(pad, cx, cy);
			wchar_t s[2];
			char mb[8];
			s[0] = txt.string[i];
			s[1] = 0;
			wcstombs(mb, s, 8);
			waddstr(pad, mb);
		}
		if(cx>=0&&cy>=0) {
			wmove(pad, cx, cy);
			pad_x = cx;
			pad_y = cy;
		} else {
			pad_x = 0;
			pad_y = 0;
		}
		wrefresh(pad);
	}

	return 0;
}
