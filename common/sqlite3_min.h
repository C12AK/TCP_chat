#ifndef SQLITE3_MIN_H
#define SQLITE3_MIN_H

// 系统里只有 libsqlite3.so.0，没有开发头文件。这里只声明本工程用到的接口。

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_OK 0            // 执行成功
#define SQLITE_ROW 100         // step 查出了一行
#define SQLITE_DONE 101        // step 做完了，没有更多行
#define SQLITE_CONSTRAINT 19   // 唯一约束等冲突，例如用户名重复
// 绑定回调：让 SQLite 自己拷贝字节。调用方的缓冲区在 bind 返回后就可以失效
#define SQLITE_TRANSIENT ((void (*)(void*))-1)

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

int sqlite3_open(const char* filename, sqlite3** ppDb);
int sqlite3_close(sqlite3*);
int sqlite3_exec(sqlite3*, const char* sql, int (*callback)(void*, int, char**, char**), void*, char** errmsg);
void sqlite3_free(void*);
const char* sqlite3_errmsg(sqlite3*);
int sqlite3_extended_errcode(sqlite3*);
long long sqlite3_last_insert_rowid(sqlite3*);

// nByte 为 -1 表示 SQL 以 NUL 结尾。prepare 只编译，不执行
int sqlite3_prepare_v2(sqlite3*, const char* sql, int nByte, sqlite3_stmt** ppStmt, const char** pzTail);
int sqlite3_step(sqlite3_stmt*);       // 执行一步
int sqlite3_reset(sqlite3_stmt*);      // 清掉上次执行的状态，好让同一条语句再绑定
int sqlite3_finalize(sqlite3_stmt*);   // 释放语句
int sqlite3_bind_int64(sqlite3_stmt*, int, long long);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int n, void (*)(void*));
int sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int n, void (*)(void*));
long long sqlite3_column_int64(sqlite3_stmt*, int iCol);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int iCol);
const void* sqlite3_column_blob(sqlite3_stmt*, int iCol);
int sqlite3_column_bytes(sqlite3_stmt*, int iCol);

#ifdef __cplusplus
}
#endif

#endif
