/*
** 2008 March 19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of SQLite interfaces.  This code
** implements new SQL functions used by the test scripts.
*/
#include "sqlite4.h"
#include "tcl.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>


/*
** Allocate nByte bytes of space using sqlite4_malloc(). If the
** allocation fails, call sqlite4_result_error_nomem() to notify
** the database handle that malloc() has failed.
*/
static void *testContextMalloc(sqlite4_context *context, int nByte){
  char *z = sqlite4_malloc(sqlite4_context_env(context), nByte);
  if( !z && nByte>0 ){
    sqlite4_result_error_nomem(context);
  }
  return z;
}

/*
** This function generates a string of random characters.  Used for
** generating test data.
*/
static void randStr(sqlite4_context *context, int argc, sqlite4_value **argv){
  static const unsigned char zSrc[] = 
     "abcdefghijklmnopqrstuvwxyz"
     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
     "0123456789"
     ".-!,:*^+=_|?/<> ";
  int iMin, iMax, n, r, i;
  sqlite4_env *pEnv = sqlite4_context_env(context);
  unsigned char zBuf[1000];

  /* It used to be possible to call randstr() with any number of arguments,
  ** but now it is registered with SQLite as requiring exactly 2.
  */
  assert(argc==2);

  iMin = sqlite4_value_int(argv[0]);
  if( iMin<0 ) iMin = 0;
  if( iMin>=sizeof(zBuf) ) iMin = sizeof(zBuf)-1;
  iMax = sqlite4_value_int(argv[1]);
  if( iMax<iMin ) iMax = iMin;
  if( iMax>=sizeof(zBuf) ) iMax = sizeof(zBuf)-1;
  n = iMin;
  if( iMax>iMin ){
    sqlite4_randomness(pEnv, sizeof(r), &r);
    r &= 0x7fffffff;
    n += r%(iMax + 1 - iMin);
  }
  assert( n<sizeof(zBuf) );
  sqlite4_randomness(pEnv, n, zBuf);
  for(i=0; i<n; i++){
    zBuf[i] = zSrc[zBuf[i]%(sizeof(zSrc)-1)];
  }
  zBuf[n] = 0;
  sqlite4_result_text(context, (char*)zBuf, n, SQLITE4_TRANSIENT, 0);
}

/*
** The following two SQL functions are used to test returning a text
** result with a destructor. Function 'test_destructor' takes one argument
** and returns the same argument interpreted as TEXT. A destructor is
** passed with the sqlite4_result_text() call.
**
** SQL function 'test_destructor_count' returns the number of outstanding 
** allocations made by 'test_destructor';
**
** WARNING: Not threadsafe.
*/
static int test_destructor_count_var = 0;
static void destructor(void *pNotUsed, void *p){
  char *zVal = (char *)p;
  assert(zVal);
  zVal--;
  sqlite4_free(0, zVal);
  test_destructor_count_var--;
}
static void test_destructor(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  char *zVal;                     /* Output value */
  const char *zText;              /* Input text */
  int nText;                      /* strlen(zText) */
  
  test_destructor_count_var++;
  assert( nArg==1 );
  if( sqlite4_value_type(argv[0])==SQLITE4_NULL ) return;
  zText = sqlite4_value_text(argv[0], &nText);
  zVal = testContextMalloc(pCtx, nText+3);
  if( !zVal ){
    return;
  }
  zVal[nText+1] = 0;
  zVal[nText+2] = 0;
  zVal++;
  memcpy(zVal, zText, nText);
  sqlite4_result_text(pCtx, zVal, -1, destructor, 0);
}
#ifndef SQLITE4_OMIT_UTF16
static void test_destructor16(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  char *zVal;                     /* Output value */
  const void *pText;              /* Input text */
  int nText;                      /* strlen(zText) */
  
  test_destructor_count_var++;
  assert( nArg==1 );
  if( sqlite4_value_type(argv[0])==SQLITE4_NULL ) return;
  pText = sqlite4_value_text16(argv[0], &nText);
  zVal = testContextMalloc(pCtx, nText+3);
  if( !zVal ){
    return;
  }
  zVal[nText+1] = 0;
  zVal[nText+2] = 0;
  zVal++;
  memcpy(zVal, pText, nText);
  sqlite4_result_text16(pCtx, zVal, -1, destructor, 0);
}
#endif
static void test_destructor_count(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  sqlite4_result_int(pCtx, test_destructor_count_var);
}

/*
** The following aggregate function, test_agg_errmsg16(), takes zero 
** arguments. It returns the text value returned by the sqlite4_errmsg16()
** API function.
*/
#ifndef SQLITE4_OMIT_BUILTIN_TEST
void sqlite4BeginBenignMalloc(void);
void sqlite4EndBenignMalloc(void);
#else
  #define sqlite4BeginBenignMalloc()
  #define sqlite4EndBenignMalloc()
#endif

/*
** Routines for testing the sqlite4_auxdata_fetch() and sqlite4_auxdata_store()
** interface.
**
** The test_auxdata() SQL function attempts to register each of its arguments
** as auxiliary data.  If there are no prior registrations of aux data for
** that argument (meaning the argument is not a constant or this is its first
** call) then the result for that argument is 0.  If there is a prior
** registration, the result for that argument is 1.  The overall result
** is the individual argument results separated by spaces.
*/
static void free_test_auxdata(void *pEnv, void *p){
  sqlite4_free((sqlite4_env*)pEnv, p);
}
static void test_auxdata(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  int i;
  char *zRet = testContextMalloc(pCtx, nArg*2);
  if( !zRet ) return;
  memset(zRet, 0, nArg*2);
  for(i=0; i<nArg; i++){
    char const *z = sqlite4_value_text(argv[i], 0);
    if( z ){
      int n;
      char *zAux = sqlite4_auxdata_fetch(pCtx, i);
      if( zAux ){
        zRet[i*2] = '1';
        assert( strcmp(zAux,z)==0 );
      }else {
        zRet[i*2] = '0';
      }
      n = strlen(z) + 1;
      zAux = testContextMalloc(pCtx, n);
      if( zAux ){
        memcpy(zAux, z, n);
        sqlite4_auxdata_store(pCtx, i, zAux,
                            free_test_auxdata, sqlite4_context_env(pCtx));
      }
      zRet[i*2+1] = ' ';
    }
  }
  sqlite4_result_text(pCtx, zRet, 2*nArg-1,
                      free_test_auxdata, sqlite4_context_env(pCtx));
}

/*
** A function to test error reporting from user functions. This function
** returns a copy of its first argument as the error message.  If the
** second argument exists, it becomes the error code.
*/
static void test_error(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  sqlite4_result_error(pCtx, sqlite4_value_text(argv[0], 0), -1);
  if( nArg==2 ){
    sqlite4_result_error_code(pCtx, sqlite4_value_int(argv[1]));
  }
}

/* A counter object with its destructor.  Used by counterFunc() below.
*/
struct counterObject { int cnt; sqlite4_env *pEnv; };
void counterFree(void *NotUsed, void *x){
  struct counterObject *p = (struct counterObject*)x;
  sqlite4_free(p->pEnv, p);
}

/*
** Implementation of the counter(X) function.  If X is an integer
** constant, then the first invocation will return X.  The second X+1.
** and so forth.  Can be used (for example) to provide a sequence number
** in a result set.
*/
static void counterFunc(
  sqlite4_context *pCtx,   /* Function context */
  int nArg,                /* Number of function arguments */
  sqlite4_value **argv     /* Values for all function arguments */
){
  struct counterObject *pCounter;
  pCounter = (struct counterObject*)sqlite4_auxdata_fetch(pCtx, 0);
  if( pCounter==0 ){
    pCounter = sqlite4_malloc(sqlite4_context_env(pCtx), sizeof(*pCounter) );
    if( pCounter==0 ){
      sqlite4_result_error_nomem(pCtx);
      return;
    }
    pCounter->cnt = sqlite4_value_int(argv[0]);
    pCounter->pEnv = sqlite4_context_env(pCtx);
    sqlite4_auxdata_store(pCtx, 0, pCounter, counterFree, 0);
  }else{
    pCounter->cnt++;
  }
  sqlite4_result_int(pCtx, pCounter->cnt);
}


/*
** This function takes two arguments.  It performance UTF-8/16 type
** conversions on the first argument then returns a copy of the second
** argument.
**
** This function is used in cases such as the following:
**
**      SELECT test_isolation(x,x) FROM t1;
**
** We want to verify that the type conversions that occur on the
** first argument do not invalidate the second argument.
*/
static void test_isolation(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
#ifndef SQLITE4_OMIT_UTF16
  sqlite4_value_text16(argv[0], 0);
  sqlite4_value_text(argv[0], 0);
  sqlite4_value_text16(argv[0], 0);
  sqlite4_value_text(argv[0], 0);
#endif
  sqlite4_result_value(pCtx, argv[1]);
}

/*
** Invoke an SQL statement recursively.  The function result is the 
** first column of the first row of the result set.
*/
static void test_eval(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  sqlite4_stmt *pStmt;
  int rc;
  sqlite4 *db = sqlite4_context_db_handle(pCtx);
  const char *zSql;

  zSql = sqlite4_value_text(argv[0], 0);
  rc = sqlite4_prepare(db, zSql, -1, &pStmt, 0);
  if( rc==SQLITE4_OK ){
    rc = sqlite4_step(pStmt);
    if( rc==SQLITE4_ROW ){
      sqlite4_result_value(pCtx, sqlite4_column_value(pStmt, 0));
    }
    rc = sqlite4_finalize(pStmt);
  }
  if( rc ){
    char *zErr;
    sqlite4_env *pEnv = sqlite4_context_env(pCtx);
    assert( pStmt==0 );
    zErr = sqlite4_mprintf(pEnv, "sqlite4_prepare() error: %s",
                           sqlite4_errmsg(db));
    sqlite4_result_text(pCtx, zErr, -1, SQLITE4_DYNAMIC, 0);
    sqlite4_result_error_code(pCtx, rc);
  }
}


/*
** convert one character from hex to binary
*/
static int testHexChar(char c){
  if( c>='0' && c<='9' ){
    return c - '0';
  }else if( c>='a' && c<='f' ){
    return c - 'a' + 10;
  }else if( c>='A' && c<='F' ){
    return c - 'A' + 10;
  }
  return 0;
}

/*
** Convert hex to binary.
*/
static void testHexToBin(const char *zIn, char *zOut){
  while( zIn[0] && zIn[1] ){
    *(zOut++) = (testHexChar(zIn[0])<<4) + testHexChar(zIn[1]);
    zIn += 2;
  }
}

/*
**      hex_to_utf16be(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using sqlite4_result_text16le().
*/
#ifndef SQLITE4_OMIT_UTF16
static void testHexToUtf16be(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  zIn = sqlite4_value_text(argv[0], &n);
  zOut = sqlite4_malloc(sqlite4_context_env(pCtx), n/2 );
  if( zOut==0 ){
    sqlite4_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    sqlite4_result_text16be(pCtx, zOut, n/2, SQLITE4_DYNAMIC, 0);
  }
}
#endif

/*
**      hex_to_utf8(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using sqlite4_result_text16le().
*/
static void testHexToUtf8(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  zIn = sqlite4_value_text(argv[0], &n);
  zOut = sqlite4_malloc(sqlite4_context_env(pCtx), n/2 );
  if( zOut==0 ){
    sqlite4_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    sqlite4_result_text(pCtx, zOut, n/2, SQLITE4_DYNAMIC, 0);
  }
}

/*
**      hex_to_utf16le(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using sqlite4_result_text16le().
*/
#ifndef SQLITE4_OMIT_UTF16
static void testHexToUtf16le(
  sqlite4_context *pCtx, 
  int nArg,
  sqlite4_value **argv
){
  int n;
  const char *zIn;
  char *zOut;
  assert( nArg==1 );
  zIn = sqlite4_value_text(argv[0], &n);
  zOut = sqlite4_malloc(sqlite4_context_env(pCtx), n/2 );
  if( zOut==0 ){
    sqlite4_result_error_nomem(pCtx);
  }else{
    testHexToBin(zIn, zOut);
    sqlite4_result_text16le(pCtx, zOut, n/2, SQLITE4_DYNAMIC, 0);
  }
}
#endif

static int registerTestFunctions(sqlite4 *db){
  static const struct {
     char *zName;
     signed char nArg;
     unsigned char eTextRep; /* 1: UTF-16.  0: UTF-8 */
     void (*xFunc)(sqlite4_context*,int,sqlite4_value **);
  } aFuncs[] = {
    { "randstr",               2, SQLITE4_UTF8, randStr    },
    { "test_destructor",       1, SQLITE4_UTF8, test_destructor},
#ifndef SQLITE4_OMIT_UTF16
    { "test_destructor16",     1, SQLITE4_UTF8, test_destructor16},
    { "hex_to_utf16be",        1, SQLITE4_UTF8, testHexToUtf16be},
    { "hex_to_utf16le",        1, SQLITE4_UTF8, testHexToUtf16le},
#endif
    { "hex_to_utf8",           1, SQLITE4_UTF8, testHexToUtf8},
    { "test_destructor_count", 0, SQLITE4_UTF8, test_destructor_count},
    { "test_auxdata",         -1, SQLITE4_UTF8, test_auxdata},
    { "test_error",            1, SQLITE4_UTF8, test_error},
    { "test_error",            2, SQLITE4_UTF8, test_error},
    { "test_eval",             1, SQLITE4_UTF8, test_eval},
    { "test_isolation",        2, SQLITE4_UTF8, test_isolation},
    { "test_counter",          1, SQLITE4_UTF8, counterFunc},
  };
  int i;

  for(i=0; i<sizeof(aFuncs)/sizeof(aFuncs[0]); i++){
    sqlite4_create_function(db, aFuncs[i].zName, aFuncs[i].nArg,
        0, aFuncs[i].xFunc, 0, 0, 0);
  }

  return SQLITE4_OK;
}

/*
** A bogus step function and finalizer function.
*/
static void tStep(sqlite4_context *a, int b, sqlite4_value **c){}
static void tFinal(sqlite4_context *a){}


/*
** tclcmd:  abuse_create_function
**
** Make various calls to sqlite4_create_function that do not have valid
** parameters.  Verify that the error condition is detected and reported.
*/
static int abuse_create_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int getDbPointer(Tcl_Interp*, const char*, sqlite4**);
  sqlite4 *db;
  int rc;
  int mxArg;

  if( getDbPointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = sqlite4_create_function(
      db, "tx", 1, 0, tStep, tStep, tFinal, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", 1, 0, tStep, tStep, 0,0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", 1, 0, tStep, 0,tFinal,0);
  if( rc!=SQLITE4_MISUSE) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", 1, 0, 0, 0, tFinal, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", 1, 0, 0, tStep, 0, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", -2, 0, tStep, 0, 0, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "tx", 128, 0, tStep, 0, 0, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  rc = sqlite4_create_function(db, "funcxx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       1, 0, tStep, 0, 0, 0);
  if( rc!=SQLITE4_MISUSE ) goto abuse_err;

  /* This last function registration should actually work.  Generate
  ** a no-op function (that always returns NULL) and which has the
  ** maximum-length function name and the maximum number of parameters.
  */
  sqlite4_limit(db, SQLITE4_LIMIT_FUNCTION_ARG, 10000);
  mxArg = sqlite4_limit(db, SQLITE4_LIMIT_FUNCTION_ARG, -1);
  rc = sqlite4_create_function(db, "nullx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       mxArg, 0, tStep, 0, 0, 0);
  if( rc!=SQLITE4_OK ) goto abuse_err;
                                
  return TCL_OK;

abuse_err:
  Tcl_AppendResult(interp, "sqlite4_create_function abused test failed", 
                   (char*)0);
  return TCL_ERROR;
}

/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest_func_Init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_ObjCmdProc *xProc;
  } aObjCmd[] = {
     { "abuse_create_function",         abuse_create_function  },
  };
  int i;

  for(i=0; i<sizeof(aObjCmd)/sizeof(aObjCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aObjCmd[i].zName, aObjCmd[i].xProc, 0, 0);
  }
  return TCL_OK;
}

int sqlite4test_install_test_functions(sqlite4 *db){
  int rc;
  extern int Md5_Register(sqlite4*);

  rc = registerTestFunctions(db);
  if( rc==SQLITE4_OK ){
    rc = Md5_Register(db);
  }
  return rc;
}
