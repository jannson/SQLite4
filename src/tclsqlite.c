/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** A TCL Interface to SQLite.  Append this file to sqlite4.c and
** compile the whole thing to build a TCL-enabled version of SQLite.
**
** Compile-time options:
**
**  -DTCLSH=1             Add a "main()" routine that works as a tclsh.
**
**  -DSQLITE4_TCLMD5       When used in conjuction with -DTCLSH=1, add
**                        four new commands to the TCL interpreter for
**                        generating MD5 checksums:  md5, md5file,
**                        md5-10x8, and md5file-10x8.
**
**  -DSQLITE4_TEST         When used in conjuction with -DTCLSH=1, add
**                        hundreds of new commands used for testing
**                        SQLite.  This option implies -DSQLITE4_TCLMD5.
*/
#include "tcl.h"
#include <errno.h>

/*
** Some additional include files are needed if this file is not
** appended to the amalgamation.
*/
#ifndef SQLITE4_AMALGAMATION
# include "sqlite4.h"
# include <stdlib.h>
# include <string.h>
# include <assert.h>
  typedef unsigned char u8;
#endif
#include <ctype.h>
#include <stdlib.h> /* atexit() */
/*
 * Windows needs to know which symbols to export.  Unix does not.
 * BUILD_sqlite should be undefined for Unix.
 */
#ifdef BUILD_sqlite
#undef TCL_STORAGE_CLASS
#define TCL_STORAGE_CLASS DLLEXPORT
#endif /* BUILD_sqlite */

#define NUM_PREPARED_STMTS 10
#define MAX_PREPARED_STMTS 100

/*
** If TCL uses UTF-8 and SQLite is configured to use iso8859, then we
** have to do a translation when going between the two.  Set the 
** UTF_TRANSLATION_NEEDED macro to indicate that we need to do
** this translation.  
*/
#if defined(TCL_UTF_MAX) && !defined(SQLITE4_UTF8)
# define UTF_TRANSLATION_NEEDED 1
#endif

/*
** New SQL functions can be created as TCL scripts.  Each such function
** is described by an instance of the following structure.
*/
typedef struct SqlFunc SqlFunc;
struct SqlFunc {
  Tcl_Interp *interp;   /* The TCL interpret to execute the function */
  Tcl_Obj *pScript;     /* The Tcl_Obj representation of the script */
  int useEvalObjv;      /* True if it is safe to use Tcl_EvalObjv */
  char *zName;          /* Name of this function */
  SqlFunc *pNext;       /* Next function on the list of them all */
};

/*
** New collation sequences function can be created as TCL scripts.  Each such
** function is described by an instance of the following structure.
*/
typedef struct SqlCollate SqlCollate;
struct SqlCollate {
  Tcl_Interp *interp;   /* The TCL interpret to execute the function */
  char *zCmp;           /* The script to compare two values */
  char *zMkkey;         /* The script to encode a key value */
  SqlCollate *pNext;    /* Next function on the list of them all */
};

/*
** Prepared statements are cached for faster execution.  Each prepared
** statement is described by an instance of the following structure.
*/
typedef struct SqlPreparedStmt SqlPreparedStmt;
struct SqlPreparedStmt {
  SqlPreparedStmt *pNext;  /* Next in linked list */
  SqlPreparedStmt *pPrev;  /* Previous on the list */
  sqlite4_stmt *pStmt;     /* The prepared statement */
  int nSql;                /* chars in zSql[] */
  const char *zSql;        /* Text of the SQL statement */
  int nParm;               /* Size of apParm array */
  Tcl_Obj **apParm;        /* Array of referenced object pointers */
};

/*
** There is one instance of this structure for each SQLite database
** that has been opened by the SQLite TCL interface.
*/
typedef struct SqliteDb SqliteDb;
struct SqliteDb {
  sqlite4 *db;               /* The "real" database structure. MUST BE FIRST */
  Tcl_Interp *interp;        /* The interpreter used for this database */
  char *zTrace;              /* The trace callback routine */
  char *zProfile;            /* The profile callback routine */
  char *zAuth;               /* The authorization callback routine */
  int disableAuth;           /* Disable the authorizer if it exists */
  char *zNull;               /* Text to substitute for an SQL NULL value */
  SqlFunc *pFunc;            /* List of SQL functions */
  SqlCollate *pCollate;      /* List of SQL collation functions */
  int rc;                    /* Return code of most recent sqlite4_exec() */
  Tcl_Obj *pCollateNeeded;   /* Collation needed script */
  SqlPreparedStmt *stmtList; /* List of prepared statements*/
  SqlPreparedStmt *stmtLast; /* Last statement in the list */
  int maxStmt;               /* The next maximum number of stmtList */
  int nStmt;                 /* Number of statements in stmtList */
  int nStep, nSort, nIndex;  /* Statistics for most recent operation */
  int nTransaction;          /* Number of nested [transaction] methods */
};

/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
*/
static int strlen30(const char *z){
  const char *z2 = z;
  while( *z2 ){ z2++; }
  return 0x3fffffff & (int)(z2 - z);
}



/*
** Look at the script prefix in pCmd.  We will be executing this script
** after first appending one or more arguments.  This routine analyzes
** the script to see if it is safe to use Tcl_EvalObjv() on the script
** rather than the more general Tcl_EvalEx().  Tcl_EvalObjv() is much
** faster.
**
** Scripts that are safe to use with Tcl_EvalObjv() consists of a
** command name followed by zero or more arguments with no [...] or $
** or {...} or ; to be seen anywhere.  Most callback scripts consist
** of just a single procedure name and they meet this requirement.
*/
static int safeToUseEvalObjv(Tcl_Interp *interp, Tcl_Obj *pCmd){
  /* We could try to do something with Tcl_Parse().  But we will instead
  ** just do a search for forbidden characters.  If any of the forbidden
  ** characters appear in pCmd, we will report the string as unsafe.
  */
  const char *z;
  int n;
  z = Tcl_GetStringFromObj(pCmd, &n);
  while( n-- > 0 ){
    int c = *(z++);
    if( c=='$' || c=='[' || c==';' ) return 0;
  }
  return 1;
}

/*
** Find an SqlFunc structure with the given name.  Or create a new
** one if an existing one cannot be found.  Return a pointer to the
** structure.
*/
static SqlFunc *findSqlFunc(SqliteDb *pDb, const char *zName){
  SqlFunc *p, *pNew;
  int i;
  pNew = (SqlFunc*)Tcl_Alloc( sizeof(*pNew) + strlen30(zName) + 1 );
  pNew->zName = (char*)&pNew[1];
  for(i=0; zName[i]; i++){ pNew->zName[i] = tolower(zName[i]); }
  pNew->zName[i] = 0;
  for(p=pDb->pFunc; p; p=p->pNext){ 
    if( strcmp(p->zName, pNew->zName)==0 ){
      Tcl_Free((char*)pNew);
      return p;
    }
  }
  pNew->interp = pDb->interp;
  pNew->pScript = 0;
  pNew->pNext = pDb->pFunc;
  pDb->pFunc = pNew;
  return pNew;
}

/*
** Free a single SqlPreparedStmt object.
*/
static void dbFreeStmt(SqlPreparedStmt *pStmt){
#ifdef SQLITE4_TEST
  if( sqlite4_stmt_sql(pStmt->pStmt)==0 ){
    Tcl_Free((char *)pStmt->zSql);
  }
#endif
  sqlite4_finalize(pStmt->pStmt);
  Tcl_Free((char *)pStmt);
}

/*
** Finalize and free a list of prepared statements
*/
static void flushStmtCache(SqliteDb *pDb){
  SqlPreparedStmt *pPreStmt;
  SqlPreparedStmt *pNext;

  for(pPreStmt = pDb->stmtList; pPreStmt; pPreStmt=pNext){
    pNext = pPreStmt->pNext;
    dbFreeStmt(pPreStmt);
  }
  pDb->nStmt = 0;
  pDb->stmtLast = 0;
  pDb->stmtList = 0;
}

/*
** TCL calls this procedure when an sqlite4 database command is
** deleted.
*/
static void DbDeleteCmd(void *db){
  SqliteDb *pDb = (SqliteDb*)db;
  flushStmtCache(pDb);
  sqlite4_close(pDb->db, 0);
  while( pDb->pFunc ){
    SqlFunc *pFunc = pDb->pFunc;
    pDb->pFunc = pFunc->pNext;
    Tcl_DecrRefCount(pFunc->pScript);
    Tcl_Free((char*)pFunc);
  }
  while( pDb->pCollate ){
    SqlCollate *pCollate = pDb->pCollate;
    pDb->pCollate = pCollate->pNext;
    Tcl_Free((char*)pCollate);
  }
  if( pDb->zTrace ){
    Tcl_Free(pDb->zTrace);
  }
  if( pDb->zProfile ){
    Tcl_Free(pDb->zProfile);
  }
  if( pDb->zAuth ){
    Tcl_Free(pDb->zAuth);
  }
  if( pDb->zNull ){
    Tcl_Free(pDb->zNull);
  }
  if( pDb->pCollateNeeded ){
    Tcl_DecrRefCount(pDb->pCollateNeeded);
  }
  Tcl_Free((char*)pDb);
}

#ifndef SQLITE4_OMIT_TRACE
/*
** This routine is called by the SQLite trace handler whenever a new
** block of SQL is executed.  The TCL script in pDb->zTrace is executed.
*/
static void DbTraceHandler(void *cd, const char *zSql){
  SqliteDb *pDb = (SqliteDb*)cd;
  Tcl_DString str;

  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, pDb->zTrace, -1);
  Tcl_DStringAppendElement(&str, zSql);
  Tcl_Eval(pDb->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  Tcl_ResetResult(pDb->interp);
}
#endif

#ifndef SQLITE4_OMIT_TRACE
/*
** This routine is called by the SQLite profile handler after a statement
** SQL has executed.  The TCL script in pDb->zProfile is evaluated.
*/
static void DbProfileHandler(void *cd, const char *zSql, sqlite4_uint64 tm){
  SqliteDb *pDb = (SqliteDb*)cd;
  Tcl_DString str;
  char zTm[100];

  sqlite4_snprintf(zTm, sizeof(zTm)-1, "%lld", tm);
  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, pDb->zProfile, -1);
  Tcl_DStringAppendElement(&str, zSql);
  Tcl_DStringAppendElement(&str, zTm);
  Tcl_Eval(pDb->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  Tcl_ResetResult(pDb->interp);
}
#endif

static void tclCollateNeeded(
  void *pCtx,
  sqlite4 *db,
  const char *zName
){
  SqliteDb *pDb = (SqliteDb *)pCtx;
  Tcl_Obj *pScript = Tcl_DuplicateObj(pDb->pCollateNeeded);
  Tcl_IncrRefCount(pScript);
  Tcl_ListObjAppendElement(0, pScript, Tcl_NewStringObj(zName, -1));
  Tcl_EvalObjEx(pDb->interp, pScript, 0);
  Tcl_DecrRefCount(pScript);
}

/*
** This routine is called to evaluate an SQL collation function implemented
** using TCL script.
*/
static int tclSqlCollate(
  void *pCtx,
  sqlite4_value *p1,
  sqlite4_value *p2,
  int *pRes
){
  int n1;
  int n2;
  const char *z1;
  const char *z2;
  SqlCollate *p = (SqlCollate *)pCtx;
  Tcl_Obj *pCmd;
  int rc;

  z1 = sqlite4_value_text(p1, &n1);
  z2 = sqlite4_value_text(p2, &n2);

  pCmd = Tcl_NewStringObj(p->zCmp, -1);
  Tcl_IncrRefCount(pCmd);
  Tcl_ListObjAppendElement(p->interp, pCmd, Tcl_NewStringObj(z1, n1));
  Tcl_ListObjAppendElement(p->interp, pCmd, Tcl_NewStringObj(z2, n2));
  rc = Tcl_EvalObjEx(p->interp, pCmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(pCmd);
  *pRes = (atoi(Tcl_GetStringResult(p->interp)));
  return (rc==TCL_OK ? SQLITE4_OK : SQLITE4_ERROR);
}

static int tclSqlMkkey(
  void *pCtx,
  sqlite4_value *pVal,
  int nOut,
  void *pOut,
  int *pnOut
){
  int nIn;
  int rc;
  const char *zIn;
  SqlCollate *p = (SqlCollate *)pCtx;
  Tcl_Obj *pCmd;
  int nRes;
  char *zRes;

  zIn = sqlite4_value_text(pVal, &nIn);

  pCmd = Tcl_NewStringObj(p->zMkkey, -1);
  Tcl_IncrRefCount(pCmd);
  Tcl_ListObjAppendElement(p->interp, pCmd, Tcl_NewStringObj(zIn, nIn));
  rc = Tcl_EvalObjEx(p->interp, pCmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount(pCmd);

  zRes = Tcl_GetStringFromObj(Tcl_GetObjResult(p->interp), &nRes);
  if( nRes<=nOut ){
    memcpy(pOut, zRes, nRes);
  }
  *pnOut = nRes;

  return rc;
}

/*
** This routine is called to evaluate an SQL function implemented
** using TCL script.
*/
static void tclSqlFunc(sqlite4_context *context, int argc, sqlite4_value**argv){
  SqlFunc *p = sqlite4_context_appdata(context);
  Tcl_Obj *pCmd;
  int i;
  int rc;

  if( argc==0 ){
    /* If there are no arguments to the function, call Tcl_EvalObjEx on the
    ** script object directly.  This allows the TCL compiler to generate
    ** bytecode for the command on the first invocation and thus make
    ** subsequent invocations much faster. */
    pCmd = p->pScript;
    Tcl_IncrRefCount(pCmd);
    rc = Tcl_EvalObjEx(p->interp, pCmd, 0);
    Tcl_DecrRefCount(pCmd);
  }else{
    /* If there are arguments to the function, make a shallow copy of the
    ** script object, lappend the arguments, then evaluate the copy.
    **
    ** By "shallow" copy, we mean a only the outer list Tcl_Obj is duplicated.
    ** The new Tcl_Obj contains pointers to the original list elements. 
    ** That way, when Tcl_EvalObjv() is run and shimmers the first element
    ** of the list to tclCmdNameType, that alternate representation will
    ** be preserved and reused on the next invocation.
    */
    Tcl_Obj **aArg;
    int nArg;
    if( Tcl_ListObjGetElements(p->interp, p->pScript, &nArg, &aArg) ){
      sqlite4_result_error(context, Tcl_GetStringResult(p->interp), -1); 
      return;
    }     
    pCmd = Tcl_NewListObj(nArg, aArg);
    Tcl_IncrRefCount(pCmd);
    for(i=0; i<argc; i++){
      sqlite4_value *pIn = argv[i];
      Tcl_Obj *pVal;
            
      /* Set pVal to contain the i'th column of this row. */
      switch( sqlite4_value_type(pIn) ){
        case SQLITE4_BLOB: {
          int nBlob;
          const void *pBlob = sqlite4_value_blob(pIn, &nBlob);
          pVal = Tcl_NewByteArrayObj(pBlob, nBlob);
          break;
        }
        case SQLITE4_INTEGER: {
          sqlite4_int64 v = sqlite4_value_int64(pIn);
          if( v>=-2147483647 && v<=2147483647 ){
            pVal = Tcl_NewIntObj((int)v);
          }else{
            pVal = Tcl_NewWideIntObj(v);
          }
          break;
        }
        case SQLITE4_FLOAT: {
          double r = sqlite4_value_double(pIn);
          pVal = Tcl_NewDoubleObj(r);
          break;
        }
        case SQLITE4_NULL: {
          pVal = Tcl_NewStringObj("", 0);
          break;
        }
        default: {
          int nText;
          const char *zText = sqlite4_value_text(pIn, &nText);
          pVal = Tcl_NewStringObj(zText, nText);
          break;
        }
      }
      rc = Tcl_ListObjAppendElement(p->interp, pCmd, pVal);
      if( rc ){
        Tcl_DecrRefCount(pCmd);
        sqlite4_result_error(context, Tcl_GetStringResult(p->interp), -1); 
        return;
      }
    }
    if( !p->useEvalObjv ){
      /* Tcl_EvalObjEx() will automatically call Tcl_EvalObjv() if pCmd
      ** is a list without a string representation.  To prevent this from
      ** happening, make sure pCmd has a valid string representation */
      Tcl_GetString(pCmd);
    }
    rc = Tcl_EvalObjEx(p->interp, pCmd, TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(pCmd);
  }

  if( rc && rc!=TCL_RETURN ){
    sqlite4_result_error(context, Tcl_GetStringResult(p->interp), -1); 
  }else{
    Tcl_Obj *pVar = Tcl_GetObjResult(p->interp);
    int n;
    u8 *data;
    const char *zType = (pVar->typePtr ? pVar->typePtr->name : "");
    char c = zType[0];
    if( c=='b' && strcmp(zType,"bytearray")==0 && pVar->bytes==0 ){
      /* Only return a BLOB type if the Tcl variable is a bytearray and
      ** has no string representation. */
      data = Tcl_GetByteArrayFromObj(pVar, &n);
      sqlite4_result_blob(context, data, n, SQLITE4_TRANSIENT, 0);
    }else if( c=='b' && strcmp(zType,"boolean")==0 ){
      Tcl_GetIntFromObj(0, pVar, &n);
      sqlite4_result_int(context, n);
    }else if( c=='d' && strcmp(zType,"double")==0 ){
      double r;
      Tcl_GetDoubleFromObj(0, pVar, &r);
      sqlite4_result_double(context, r);
    }else if( (c=='w' && strcmp(zType,"wideInt")==0) ||
          (c=='i' && strcmp(zType,"int")==0) ){
      Tcl_WideInt v;
      Tcl_GetWideIntFromObj(0, pVar, &v);
      sqlite4_result_int64(context, v);
    }else{
      data = (unsigned char *)Tcl_GetStringFromObj(pVar, &n);
      sqlite4_result_text(context, (char *)data, n, SQLITE4_TRANSIENT, 0);
    }
  }
}

#ifndef SQLITE4_OMIT_AUTHORIZATION
/*
** This is the authentication function.  It appends the authentication
** type code and the two arguments to zCmd[] then invokes the result
** on the interpreter.  The reply is examined to determine if the
** authentication fails or succeeds.
*/
static int auth_callback(
  void *pArg,
  int code,
  const char *zArg1,
  const char *zArg2,
  const char *zArg3,
  const char *zArg4
){
  char *zCode;
  Tcl_DString str;
  int rc;
  const char *zReply;
  SqliteDb *pDb = (SqliteDb*)pArg;
  if( pDb->disableAuth ) return SQLITE4_OK;

  switch( code ){
    case SQLITE4_COPY              : zCode="SQLITE4_COPY"; break;
    case SQLITE4_CREATE_INDEX      : zCode="SQLITE4_CREATE_INDEX"; break;
    case SQLITE4_CREATE_TABLE      : zCode="SQLITE4_CREATE_TABLE"; break;
    case SQLITE4_CREATE_TEMP_INDEX : zCode="SQLITE4_CREATE_TEMP_INDEX"; break;
    case SQLITE4_CREATE_TEMP_TABLE : zCode="SQLITE4_CREATE_TEMP_TABLE"; break;
    case SQLITE4_CREATE_TEMP_TRIGGER: zCode="SQLITE4_CREATE_TEMP_TRIGGER"; break;
    case SQLITE4_CREATE_TEMP_VIEW  : zCode="SQLITE4_CREATE_TEMP_VIEW"; break;
    case SQLITE4_CREATE_TRIGGER    : zCode="SQLITE4_CREATE_TRIGGER"; break;
    case SQLITE4_CREATE_VIEW       : zCode="SQLITE4_CREATE_VIEW"; break;
    case SQLITE4_DELETE            : zCode="SQLITE4_DELETE"; break;
    case SQLITE4_DROP_INDEX        : zCode="SQLITE4_DROP_INDEX"; break;
    case SQLITE4_DROP_TABLE        : zCode="SQLITE4_DROP_TABLE"; break;
    case SQLITE4_DROP_TEMP_INDEX   : zCode="SQLITE4_DROP_TEMP_INDEX"; break;
    case SQLITE4_DROP_TEMP_TABLE   : zCode="SQLITE4_DROP_TEMP_TABLE"; break;
    case SQLITE4_DROP_TEMP_TRIGGER : zCode="SQLITE4_DROP_TEMP_TRIGGER"; break;
    case SQLITE4_DROP_TEMP_VIEW    : zCode="SQLITE4_DROP_TEMP_VIEW"; break;
    case SQLITE4_DROP_TRIGGER      : zCode="SQLITE4_DROP_TRIGGER"; break;
    case SQLITE4_DROP_VIEW         : zCode="SQLITE4_DROP_VIEW"; break;
    case SQLITE4_INSERT            : zCode="SQLITE4_INSERT"; break;
    case SQLITE4_PRAGMA            : zCode="SQLITE4_PRAGMA"; break;
    case SQLITE4_READ              : zCode="SQLITE4_READ"; break;
    case SQLITE4_SELECT            : zCode="SQLITE4_SELECT"; break;
    case SQLITE4_TRANSACTION       : zCode="SQLITE4_TRANSACTION"; break;
    case SQLITE4_UPDATE            : zCode="SQLITE4_UPDATE"; break;
    case SQLITE4_ATTACH            : zCode="SQLITE4_ATTACH"; break;
    case SQLITE4_DETACH            : zCode="SQLITE4_DETACH"; break;
    case SQLITE4_ALTER_TABLE       : zCode="SQLITE4_ALTER_TABLE"; break;
    case SQLITE4_REINDEX           : zCode="SQLITE4_REINDEX"; break;
    case SQLITE4_ANALYZE           : zCode="SQLITE4_ANALYZE"; break;
    case SQLITE4_CREATE_VTABLE     : zCode="SQLITE4_CREATE_VTABLE"; break;
    case SQLITE4_DROP_VTABLE       : zCode="SQLITE4_DROP_VTABLE"; break;
    case SQLITE4_FUNCTION          : zCode="SQLITE4_FUNCTION"; break;
    case SQLITE4_SAVEPOINT         : zCode="SQLITE4_SAVEPOINT"; break;
    default                       : zCode="????"; break;
  }
  Tcl_DStringInit(&str);
  Tcl_DStringAppend(&str, pDb->zAuth, -1);
  Tcl_DStringAppendElement(&str, zCode);
  Tcl_DStringAppendElement(&str, zArg1 ? zArg1 : "");
  Tcl_DStringAppendElement(&str, zArg2 ? zArg2 : "");
  Tcl_DStringAppendElement(&str, zArg3 ? zArg3 : "");
  Tcl_DStringAppendElement(&str, zArg4 ? zArg4 : "");
  rc = Tcl_GlobalEval(pDb->interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  zReply = rc==TCL_OK ? Tcl_GetStringResult(pDb->interp) : "SQLITE4_DENY";
  if( strcmp(zReply,"SQLITE4_OK")==0 ){
    rc = SQLITE4_OK;
  }else if( strcmp(zReply,"SQLITE4_DENY")==0 ){
    rc = SQLITE4_DENY;
  }else if( strcmp(zReply,"SQLITE4_IGNORE")==0 ){
    rc = SQLITE4_IGNORE;
  }else{
    rc = 999;
  }
  return rc;
}
#endif /* SQLITE4_OMIT_AUTHORIZATION */

/*
** zText is a pointer to text obtained via an sqlite4_result_text()
** or similar interface. This routine returns a Tcl string object, 
** reference count set to 0, containing the text. If a translation
** between iso8859 and UTF-8 is required, it is preformed.
*/
static Tcl_Obj *dbTextToObj(char const *zText){
  Tcl_Obj *pVal;
#ifdef UTF_TRANSLATION_NEEDED
  Tcl_DString dCol;
  Tcl_DStringInit(&dCol);
  Tcl_ExternalToUtfDString(NULL, zText, -1, &dCol);
  pVal = Tcl_NewStringObj(Tcl_DStringValue(&dCol), -1);
  Tcl_DStringFree(&dCol);
#else
  pVal = Tcl_NewStringObj(zText, -1);
#endif
  return pVal;
}

/*
** This routine reads a line of text from FILE in, stores
** the text in memory obtained from malloc() and returns a pointer
** to the text.  NULL is returned at end of file, or if malloc()
** fails.
**
** The interface is like "readline" but no command-line editing
** is done.
**
** copied from shell.c from '.import' command
*/
static char *local_getline(char *zPrompt, FILE *in){
  char *zLine;
  int nLine;
  int n;

  nLine = 100;
  zLine = malloc( nLine );
  if( zLine==0 ) return 0;
  n = 0;
  while( 1 ){
    if( n+100>nLine ){
      nLine = nLine*2 + 100;
      zLine = realloc(zLine, nLine);
      if( zLine==0 ) return 0;
    }
    if( fgets(&zLine[n], nLine - n, in)==0 ){
      if( n==0 ){
        free(zLine);
        return 0;
      }
      zLine[n] = 0;
      break;
    }
    while( zLine[n] ){ n++; }
    if( n>0 && zLine[n-1]=='\n' ){
      n--;
      zLine[n] = 0;
      break;
    }
  }
  zLine = realloc( zLine, n+1 );
  return zLine;
}


/*
** This function is part of the implementation of the command:
**
**   $db transaction [-deferred|-immediate|-exclusive] SCRIPT
**
** It is invoked after evaluating the script SCRIPT to commit or rollback
** the transaction or savepoint opened by the [transaction] command.
*/
static int DbTransPostCmd(
  ClientData data[],                   /* data[0] is the Sqlite3Db* for $db */
  Tcl_Interp *interp,                  /* Tcl interpreter */
  int result                           /* Result of evaluating SCRIPT */
){
  static const char *azEnd[] = {
    "RELEASE _tcl_transaction",        /* rc==TCL_ERROR, nTransaction!=0 */
    "COMMIT",                          /* rc!=TCL_ERROR, nTransaction==0 */
    "ROLLBACK TO _tcl_transaction ; RELEASE _tcl_transaction",
    "ROLLBACK"                         /* rc==TCL_ERROR, nTransaction==0 */
  };
  SqliteDb *pDb = (SqliteDb*)data[0];
  int rc = result;
  const char *zEnd;

  pDb->nTransaction--;
  zEnd = azEnd[(rc==TCL_ERROR)*2 + (pDb->nTransaction==0)];

  pDb->disableAuth++;
  if( sqlite4_exec(pDb->db, zEnd, 0, 0) ){
      /* This is a tricky scenario to handle. The most likely cause of an
      ** error is that the exec() above was an attempt to commit the 
      ** top-level transaction that returned SQLITE4_BUSY. Or, less likely,
      ** that an IO-error has occured. In either case, throw a Tcl exception
      ** and try to rollback the transaction.
      **
      ** But it could also be that the user executed one or more BEGIN, 
      ** COMMIT, SAVEPOINT, RELEASE or ROLLBACK commands that are confusing
      ** this method's logic. Not clear how this would be best handled.
      */
    if( rc!=TCL_ERROR ){
      Tcl_AppendResult(interp, sqlite4_errmsg(pDb->db), 0);
      rc = TCL_ERROR;
    }
    sqlite4_exec(pDb->db, "ROLLBACK", 0, 0);
  }
  pDb->disableAuth--;

  return rc;
}

/*
** Search the cache for a prepared-statement object that implements the
** first SQL statement in the buffer pointed to by parameter zIn. If
** no such prepared-statement can be found, allocate and prepare a new
** one. In either case, bind the current values of the relevant Tcl
** variables to any $var, :var or @var variables in the statement. Before
** returning, set *ppPreStmt to point to the prepared-statement object.
**
** Output parameter *pzOut is set to point to the next SQL statement in
** buffer zIn, or to the '\0' byte at the end of zIn if there is no
** next statement.
**
** If successful, TCL_OK is returned. Otherwise, TCL_ERROR is returned
** and an error message loaded into interpreter pDb->interp.
*/
static int dbPrepareAndBind(
  SqliteDb *pDb,                  /* Database object */
  char const *zIn,                /* SQL to compile */
  char const **pzOut,             /* OUT: Pointer to next SQL statement */
  SqlPreparedStmt **ppPreStmt     /* OUT: Object used to cache statement */
){
  const char *zSql = zIn;         /* Pointer to first SQL statement in zIn */
  sqlite4_stmt *pStmt;            /* Prepared statement object */
  SqlPreparedStmt *pPreStmt;      /* Pointer to cached statement */
  int nSql;                       /* Length of zSql in bytes */
  int nVar;                       /* Number of variables in statement */
  int iParm = 0;                  /* Next free entry in apParm */
  int i;
  Tcl_Interp *interp = pDb->interp;

  *ppPreStmt = 0;

  /* Trim spaces from the start of zSql and calculate the remaining length. */
  while( isspace(zSql[0]) ){ zSql++; }
  nSql = strlen30(zSql);

  for(pPreStmt = pDb->stmtList; pPreStmt; pPreStmt=pPreStmt->pNext){
    int n = pPreStmt->nSql;
    if( nSql>=n 
        && memcmp(pPreStmt->zSql, zSql, n)==0
        && (zSql[n]==0 || zSql[n-1]==';')
    ){
      pStmt = pPreStmt->pStmt;
      *pzOut = &zSql[pPreStmt->nSql];

      /* When a prepared statement is found, unlink it from the
      ** cache list.  It will later be added back to the beginning
      ** of the cache list in order to implement LRU replacement.
      */
      if( pPreStmt->pPrev ){
        pPreStmt->pPrev->pNext = pPreStmt->pNext;
      }else{
        pDb->stmtList = pPreStmt->pNext;
      }
      if( pPreStmt->pNext ){
        pPreStmt->pNext->pPrev = pPreStmt->pPrev;
      }else{
        pDb->stmtLast = pPreStmt->pPrev;
      }
      pDb->nStmt--;
      nVar = sqlite4_bind_parameter_count(pStmt);
      break;
    }
  }
  
  /* If no prepared statement was found. Compile the SQL text. Also allocate
  ** a new SqlPreparedStmt structure.  */
  if( pPreStmt==0 ){
    int nByte;
    int nUsed;

    if( SQLITE4_OK!=sqlite4_prepare(pDb->db, zSql, -1, &pStmt, &nUsed) ){
      Tcl_SetObjResult(interp, dbTextToObj(sqlite4_errmsg(pDb->db)));
      return TCL_ERROR;
    }
    *pzOut = &zSql[nUsed];
    if( pStmt==0 ){
      if( SQLITE4_OK!=sqlite4_errcode(pDb->db) ){
        /* A compile-time error in the statement. */
        Tcl_SetObjResult(interp, dbTextToObj(sqlite4_errmsg(pDb->db)));
        return TCL_ERROR;
      }else{
        /* The statement was a no-op.  Continue to the next statement
        ** in the SQL string.
        */
        return TCL_OK;
      }
    }

    assert( pPreStmt==0 );
    nVar = sqlite4_bind_parameter_count(pStmt);
    nByte = sizeof(SqlPreparedStmt) + nVar*sizeof(Tcl_Obj *);
    pPreStmt = (SqlPreparedStmt*)Tcl_Alloc(nByte);
    memset(pPreStmt, 0, nByte);

    pPreStmt->pStmt = pStmt;
    pPreStmt->nSql = nUsed;
    pPreStmt->zSql = sqlite4_stmt_sql(pStmt);
    pPreStmt->apParm = (Tcl_Obj **)&pPreStmt[1];
#ifdef SQLITE4_TEST
    if( pPreStmt->zSql==0 ){
      char *zCopy = Tcl_Alloc(pPreStmt->nSql + 1);
      memcpy(zCopy, zSql, pPreStmt->nSql);
      zCopy[pPreStmt->nSql] = '\0';
      pPreStmt->zSql = zCopy;
    }
#endif
  }
  assert( pPreStmt );
  assert( strlen30(pPreStmt->zSql)==pPreStmt->nSql );
  assert( 0==memcmp(pPreStmt->zSql, zSql, pPreStmt->nSql) );

  /* Bind values to parameters that begin with $ or : */  
  for(i=1; i<=nVar; i++){
    const char *zVar = sqlite4_bind_parameter_name(pStmt, i);
    if( zVar!=0 && (zVar[0]=='$' || zVar[0]==':' || zVar[0]=='@') ){
      Tcl_Obj *pVar = Tcl_GetVar2Ex(interp, &zVar[1], 0, 0);
      if( pVar ){
        int n;
        u8 *data;
        const char *zType = (pVar->typePtr ? pVar->typePtr->name : "");
        char c = zType[0];
        if( zVar[0]=='@' ||
           (c=='b' && strcmp(zType,"bytearray")==0 && pVar->bytes==0) ){
          /* Load a BLOB type if the Tcl variable is a bytearray and
          ** it has no string representation or the host
          ** parameter name begins with "@". */
          data = Tcl_GetByteArrayFromObj(pVar, &n);
          sqlite4_bind_blob(pStmt, i, data, n, SQLITE4_STATIC, 0);
          Tcl_IncrRefCount(pVar);
          pPreStmt->apParm[iParm++] = pVar;
        }else if( c=='b' && strcmp(zType,"boolean")==0 ){
          Tcl_GetIntFromObj(interp, pVar, &n);
          sqlite4_bind_int(pStmt, i, n);
        }else if( c=='d' && strcmp(zType,"double")==0 ){
          double r;
          Tcl_GetDoubleFromObj(interp, pVar, &r);
          sqlite4_bind_double(pStmt, i, r);
        }else if( (c=='w' && strcmp(zType,"wideInt")==0) ||
              (c=='i' && strcmp(zType,"int")==0) ){
          Tcl_WideInt v;
          Tcl_GetWideIntFromObj(interp, pVar, &v);
          sqlite4_bind_int64(pStmt, i, v);
        }else{
          data = (unsigned char *)Tcl_GetStringFromObj(pVar, &n);
          sqlite4_bind_text(pStmt, i, (char *)data, n, SQLITE4_STATIC, 0);
          Tcl_IncrRefCount(pVar);
          pPreStmt->apParm[iParm++] = pVar;
        }
      }else{
        sqlite4_bind_null(pStmt, i);
      }
    }
  }
  pPreStmt->nParm = iParm;
  *ppPreStmt = pPreStmt;

  return TCL_OK;
}

/*
** Release a statement reference obtained by calling dbPrepareAndBind().
** There should be exactly one call to this function for each call to
** dbPrepareAndBind().
**
** If the discard parameter is non-zero, then the statement is deleted
** immediately. Otherwise it is added to the LRU list and may be returned
** by a subsequent call to dbPrepareAndBind().
*/
static void dbReleaseStmt(
  SqliteDb *pDb,                  /* Database handle */
  SqlPreparedStmt *pPreStmt,      /* Prepared statement handle to release */
  int discard                     /* True to delete (not cache) the pPreStmt */
){
  int i;

  /* Free the bound string and blob parameters */
  for(i=0; i<pPreStmt->nParm; i++){
    Tcl_DecrRefCount(pPreStmt->apParm[i]);
  }
  pPreStmt->nParm = 0;

  if( pDb->maxStmt<=0 || discard ){
    /* If the cache is turned off, deallocated the statement */
    dbFreeStmt(pPreStmt);
  }else{
    /* Add the prepared statement to the beginning of the cache list. */
    pPreStmt->pNext = pDb->stmtList;
    pPreStmt->pPrev = 0;
    if( pDb->stmtList ){
     pDb->stmtList->pPrev = pPreStmt;
    }
    pDb->stmtList = pPreStmt;
    if( pDb->stmtLast==0 ){
      assert( pDb->nStmt==0 );
      pDb->stmtLast = pPreStmt;
    }else{
      assert( pDb->nStmt>0 );
    }
    pDb->nStmt++;
   
    /* If we have too many statement in cache, remove the surplus from 
    ** the end of the cache list.  */
    while( pDb->nStmt>pDb->maxStmt ){
      SqlPreparedStmt *pLast = pDb->stmtLast;
      pDb->stmtLast = pLast->pPrev;
      pDb->stmtLast->pNext = 0;
      pDb->nStmt--;
      dbFreeStmt(pLast);
    }
  }
}

/*
** Structure used with dbEvalXXX() functions:
**
**   dbEvalInit()
**   dbEvalStep()
**   dbEvalFinalize()
**   dbEvalRowInfo()
**   dbEvalColumnValue()
*/
typedef struct DbEvalContext DbEvalContext;
struct DbEvalContext {
  SqliteDb *pDb;                  /* Database handle */
  Tcl_Obj *pSql;                  /* Object holding string zSql */
  const char *zSql;               /* Remaining SQL to execute */
  SqlPreparedStmt *pPreStmt;      /* Current statement */
  int nCol;                       /* Number of columns returned by pStmt */
  Tcl_Obj *pArray;                /* Name of array variable */
  Tcl_Obj **apColName;            /* Array of column names */
};

/*
** Release any cache of column names currently held as part of
** the DbEvalContext structure passed as the first argument.
*/
static void dbReleaseColumnNames(DbEvalContext *p){
  if( p->apColName ){
    int i;
    for(i=0; i<p->nCol; i++){
      Tcl_DecrRefCount(p->apColName[i]);
    }
    Tcl_Free((char *)p->apColName);
    p->apColName = 0;
  }
  p->nCol = 0;
}

/*
** Initialize a DbEvalContext structure.
**
** If pArray is not NULL, then it contains the name of a Tcl array
** variable. The "*" member of this array is set to a list containing
** the names of the columns returned by the statement as part of each
** call to dbEvalStep(), in order from left to right. e.g. if the names 
** of the returned columns are a, b and c, it does the equivalent of the 
** tcl command:
**
**     set ${pArray}(*) {a b c}
*/
static void dbEvalInit(
  DbEvalContext *p,               /* Pointer to structure to initialize */
  SqliteDb *pDb,                  /* Database handle */
  Tcl_Obj *pSql,                  /* Object containing SQL script */
  Tcl_Obj *pArray                 /* Name of Tcl array to set (*) element of */
){
  memset(p, 0, sizeof(DbEvalContext));
  p->pDb = pDb;
  p->zSql = Tcl_GetString(pSql);
  p->pSql = pSql;
  Tcl_IncrRefCount(pSql);
  if( pArray ){
    p->pArray = pArray;
    Tcl_IncrRefCount(pArray);
  }
}

/*
** Obtain information about the row that the DbEvalContext passed as the
** first argument currently points to.
*/
static void dbEvalRowInfo(
  DbEvalContext *p,               /* Evaluation context */
  int *pnCol,                     /* OUT: Number of column names */
  Tcl_Obj ***papColName           /* OUT: Array of column names */
){
  /* Compute column names */
  if( 0==p->apColName ){
    sqlite4_stmt *pStmt = p->pPreStmt->pStmt;
    int i;                        /* Iterator variable */
    int nCol;                     /* Number of columns returned by pStmt */
    Tcl_Obj **apColName = 0;      /* Array of column names */

    p->nCol = nCol = sqlite4_column_count(pStmt);
    if( nCol>0 && (papColName || p->pArray) ){
      apColName = (Tcl_Obj**)Tcl_Alloc( sizeof(Tcl_Obj*)*nCol );
      for(i=0; i<nCol; i++){
        apColName[i] = dbTextToObj(sqlite4_column_name(pStmt,i));
        Tcl_IncrRefCount(apColName[i]);
      }
      p->apColName = apColName;
    }

    /* If results are being stored in an array variable, then create
    ** the array(*) entry for that array
    */
    if( p->pArray ){
      Tcl_Interp *interp = p->pDb->interp;
      Tcl_Obj *pColList = Tcl_NewObj();
      Tcl_Obj *pStar = Tcl_NewStringObj("*", -1);

      for(i=0; i<nCol; i++){
        Tcl_ListObjAppendElement(interp, pColList, apColName[i]);
      }
      Tcl_IncrRefCount(pStar);
      Tcl_ObjSetVar2(interp, p->pArray, pStar, pColList, 0);
      Tcl_DecrRefCount(pStar);
    }
  }

  if( papColName ){
    *papColName = p->apColName;
  }
  if( pnCol ){
    *pnCol = p->nCol;
  }
}

/*
** Return one of TCL_OK, TCL_BREAK or TCL_ERROR. If TCL_ERROR is
** returned, then an error message is stored in the interpreter before
** returning.
**
** A return value of TCL_OK means there is a row of data available. The
** data may be accessed using dbEvalRowInfo() and dbEvalColumnValue(). This
** is analogous to a return of SQLITE4_ROW from sqlite4_step(). If TCL_BREAK
** is returned, then the SQL script has finished executing and there are
** no further rows available. This is similar to SQLITE4_DONE.
*/
static int dbEvalStep(DbEvalContext *p){
  const char *zPrevSql = 0;       /* Previous value of p->zSql */

  while( p->zSql[0] || p->pPreStmt ){
    int rc;
    if( p->pPreStmt==0 ){
      zPrevSql = (p->zSql==zPrevSql ? 0 : p->zSql);
      rc = dbPrepareAndBind(p->pDb, p->zSql, &p->zSql, &p->pPreStmt);
      if( rc!=TCL_OK ) return rc;
    }else{
      int rcs;
      SqliteDb *pDb = p->pDb;
      SqlPreparedStmt *pPreStmt = p->pPreStmt;
      sqlite4_stmt *pStmt = pPreStmt->pStmt;

      rcs = sqlite4_step(pStmt);
      if( rcs==SQLITE4_ROW ){
        return TCL_OK;
      }
      if( p->pArray ){
        dbEvalRowInfo(p, 0, 0);
      }
      rcs = sqlite4_reset(pStmt);

      pDb->nStep = sqlite4_stmt_status(pStmt,SQLITE4_STMTSTATUS_FULLSCAN_STEP,1);
      pDb->nSort = sqlite4_stmt_status(pStmt,SQLITE4_STMTSTATUS_SORT,1);
      pDb->nIndex = sqlite4_stmt_status(pStmt,SQLITE4_STMTSTATUS_AUTOINDEX,1);
      dbReleaseColumnNames(p);
      p->pPreStmt = 0;

      if( rcs!=SQLITE4_OK ){
        /* If a run-time error occurs, report the error and stop reading
        ** the SQL.  */
        dbReleaseStmt(pDb, pPreStmt, 1);
        Tcl_SetObjResult(pDb->interp, dbTextToObj(sqlite4_errmsg(pDb->db)));
        return TCL_ERROR;
      }else{
        dbReleaseStmt(pDb, pPreStmt, 0);
      }
    }
  }

  /* Finished */
  return TCL_BREAK;
}

/*
** Free all resources currently held by the DbEvalContext structure passed
** as the first argument. There should be exactly one call to this function
** for each call to dbEvalInit().
*/
static void dbEvalFinalize(DbEvalContext *p){
  if( p->pPreStmt ){
    sqlite4_reset(p->pPreStmt->pStmt);
    dbReleaseStmt(p->pDb, p->pPreStmt, 0);
    p->pPreStmt = 0;
  }
  if( p->pArray ){
    Tcl_DecrRefCount(p->pArray);
    p->pArray = 0;
  }
  Tcl_DecrRefCount(p->pSql);
  dbReleaseColumnNames(p);
}

/*
** Return a pointer to a Tcl_Obj structure with ref-count 0 that contains
** the value for the iCol'th column of the row currently pointed to by
** the DbEvalContext structure passed as the first argument.
*/
static Tcl_Obj *dbEvalColumnValue(DbEvalContext *p, int iCol){
  sqlite4_stmt *pStmt = p->pPreStmt->pStmt;
  switch( sqlite4_column_type(pStmt, iCol) ){
    case SQLITE4_BLOB: {
      int bytes;
      const char *zBlob = sqlite4_column_blob(pStmt, iCol, &bytes);
      if( !zBlob ) bytes = 0;
      return Tcl_NewByteArrayObj((u8*)zBlob, bytes);
    }
    case SQLITE4_INTEGER: {
      sqlite4_int64 v = sqlite4_column_int64(pStmt, iCol);
      if( v>=-2147483647 && v<=2147483647 ){
        return Tcl_NewIntObj((int)v);
      }else{
        return Tcl_NewWideIntObj(v);
      }
    }
    case SQLITE4_FLOAT: {
      return Tcl_NewDoubleObj(sqlite4_column_double(pStmt, iCol));
    }
    case SQLITE4_NULL: {
      return dbTextToObj(p->pDb->zNull);
    }
  }

  return dbTextToObj(sqlite4_column_text(pStmt, iCol, 0));
}

/*
** If using Tcl version 8.6 or greater, use the NR functions to avoid
** recursive evalution of scripts by the [db eval] and [db trans]
** commands. Even if the headers used while compiling the extension
** are 8.6 or newer, the code still tests the Tcl version at runtime.
** This allows stubs-enabled builds to be used with older Tcl libraries.
*/
#if TCL_MAJOR_VERSION>8 || (TCL_MAJOR_VERSION==8 && TCL_MINOR_VERSION>=6)
# define SQLITE4_TCL_NRE 1
static int DbUseNre(void){
  int major, minor;
  Tcl_GetVersion(&major, &minor, 0, 0);
  return( (major==8 && minor>=6) || major>8 );
}
#else
/* 
** Compiling using headers earlier than 8.6. In this case NR cannot be
** used, so DbUseNre() to always return zero. Add #defines for the other
** Tcl_NRxxx() functions to prevent them from causing compilation errors,
** even though the only invocations of them are within conditional blocks 
** of the form:
**
**   if( DbUseNre() ) { ... }
*/
# define SQLITE4_TCL_NRE 0
# define DbUseNre() 0
# define Tcl_NRAddCallback(a,b,c,d,e,f) 0
# define Tcl_NREvalObj(a,b,c) 0
# define Tcl_NRCreateCommand(a,b,c,d,e,f) 0
#endif

/*
** This function is part of the implementation of the command:
**
**   $db eval SQL ?ARRAYNAME? SCRIPT
*/
static int DbEvalNextCmd(
  ClientData data[],                   /* data[0] is the (DbEvalContext*) */
  Tcl_Interp *interp,                  /* Tcl interpreter */
  int result                           /* Result so far */
){
  int rc = result;                     /* Return code */

  /* The first element of the data[] array is a pointer to a DbEvalContext
  ** structure allocated using Tcl_Alloc(). The second element of data[]
  ** is a pointer to a Tcl_Obj containing the script to run for each row
  ** returned by the queries encapsulated in data[0]. */
  DbEvalContext *p = (DbEvalContext *)data[0];
  Tcl_Obj *pScript = (Tcl_Obj *)data[1];
  Tcl_Obj *pArray = p->pArray;

  while( (rc==TCL_OK || rc==TCL_CONTINUE) && TCL_OK==(rc = dbEvalStep(p)) ){
    int i;
    int nCol;
    Tcl_Obj **apColName;
    dbEvalRowInfo(p, &nCol, &apColName);
    for(i=0; i<nCol; i++){
      Tcl_Obj *pVal = dbEvalColumnValue(p, i);
      if( pArray==0 ){
        Tcl_ObjSetVar2(interp, apColName[i], 0, pVal, 0);
      }else{
        Tcl_ObjSetVar2(interp, pArray, apColName[i], pVal, 0);
      }
    }

    /* The required interpreter variables are now populated with the data 
    ** from the current row. If using NRE, schedule callbacks to evaluate
    ** script pScript, then to invoke this function again to fetch the next
    ** row (or clean up if there is no next row or the script throws an
    ** exception). After scheduling the callbacks, return control to the 
    ** caller.
    **
    ** If not using NRE, evaluate pScript directly and continue with the
    ** next iteration of this while(...) loop.  */
    if( DbUseNre() ){
      Tcl_NRAddCallback(interp, DbEvalNextCmd, (void*)p, (void*)pScript, 0, 0);
      return Tcl_NREvalObj(interp, pScript, 0);
    }else{
      rc = Tcl_EvalObjEx(interp, pScript, 0);
    }
  }

  Tcl_DecrRefCount(pScript);
  dbEvalFinalize(p);
  Tcl_Free((char *)p);

  if( rc==TCL_OK || rc==TCL_BREAK ){
    Tcl_ResetResult(interp);
    rc = TCL_OK;
  }
  return rc;
}

/*
** The "sqlite" command below creates a new Tcl command for each
** connection it opens to an SQLite database.  This routine is invoked
** whenever one of those connection-specific commands is executed
** in Tcl.  For example, if you run Tcl code like this:
**
**       sqlite4 db1  "my_database"
**       db1 close
**
** The first command opens a connection to the "my_database" database
** and calls that connection "db1".  The second command causes this
** subroutine to be invoked.
*/
static int DbObjCmd(void *cd, Tcl_Interp *interp, int objc,Tcl_Obj *const*objv){
  SqliteDb *pDb = (SqliteDb*)cd;
  int choice;
  int rc = TCL_OK;
  static const char *DB_strs[] = {
    "authorizer",         "cache",             "changes",
    "close",              "collate",           "collation_needed",
    "complete",           "copy",              "enable_load_extension", 
    "errorcode",          "eval",              "exists",             
    "function",           "interrupt",         
    "nullvalue",          "onecolumn",         "profile",
    "rekey",              "status",            "total_changes",
    "trace",              "transaction",
    "version",            0
  };
  enum DB_enum {
    DB_AUTHORIZER,        DB_CACHE,            DB_CHANGES,
    DB_CLOSE,             DB_COLLATE,          DB_COLLATION_NEEDED,
    DB_COMPLETE,          DB_COPY,             DB_ENABLE_LOAD_EXTENSION, 
    DB_ERRORCODE,         DB_EVAL,             DB_EXISTS,            
    DB_FUNCTION,          DB_INTERRUPT,        
    DB_NULLVALUE,         DB_ONECOLUMN,        DB_PROFILE,           
    DB_REKEY,             DB_STATUS,           DB_TOTAL_CHANGES,    
    DB_TRACE,             DB_TRANSACTION,
    DB_VERSION
  };
  /* don't leave trailing commas on DB_enum, it confuses the AIX xlc compiler */

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  if( Tcl_GetIndexFromObj(interp, objv[1], DB_strs, "option", 0, &choice) ){
    return TCL_ERROR;
  }

  switch( (enum DB_enum)choice ){

  /*    $db authorizer ?CALLBACK?
  **
  ** Invoke the given callback to authorize each SQL operation as it is
  ** compiled.  5 arguments are appended to the callback before it is
  ** invoked:
  **
  **   (1) The authorization type (ex: SQLITE4_CREATE_TABLE, SQLITE4_READ, ...)
  **   (2) First descriptive name (depends on authorization type)
  **   (3) Second descriptive name
  **   (4) Name of the database (ex: "main", "temp")
  **   (5) Name of trigger that is doing the access
  **
  ** The callback should return on of the following strings: SQLITE4_OK,
  ** SQLITE4_IGNORE, or SQLITE4_DENY.  Any other return value is an error.
  **
  ** If this method is invoked with no arguments, the current authorization
  ** callback string is returned.
  */
  case DB_AUTHORIZER: {
#ifdef SQLITE4_OMIT_AUTHORIZATION
    Tcl_AppendResult(interp, "authorization not available in this build", 0);
    return TCL_ERROR;
#else
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( pDb->zAuth ){
        Tcl_AppendResult(interp, pDb->zAuth, 0);
      }
    }else{
      char *zAuth;
      int len;
      if( pDb->zAuth ){
        Tcl_Free(pDb->zAuth);
        sqlite4_authorizer_pop(pDb->db);
      }
      zAuth = Tcl_GetStringFromObj(objv[2], &len);
      if( zAuth && len>0 ){
        pDb->zAuth = Tcl_Alloc( len + 1 );
        memcpy(pDb->zAuth, zAuth, len+1);
      }else{
        pDb->zAuth = 0;
      }
      if( pDb->zAuth ){
        pDb->interp = interp;
        rc = sqlite4_authorizer_push(pDb->db, pDb, auth_callback, 0);
        if( rc!=SQLITE4_OK ){
          Tcl_SetResult(interp, (char *)sqlite4_errmsg(pDb->db), TCL_VOLATILE);
          return TCL_ERROR;
        }
      }
    }
#endif
    break;
  }

  /*     $db cache flush
  **     $db cache size n
  **
  ** Flush the prepared statement cache, or set the maximum number of
  ** cached statements.
  */
  case DB_CACHE: {
    char *subCmd;
    int n;

    if( objc<=2 ){
      Tcl_WrongNumArgs(interp, 1, objv, "cache option ?arg?");
      return TCL_ERROR;
    }
    subCmd = Tcl_GetStringFromObj( objv[2], 0 );
    if( *subCmd=='f' && strcmp(subCmd,"flush")==0 ){
      if( objc!=3 ){
        Tcl_WrongNumArgs(interp, 2, objv, "flush");
        return TCL_ERROR;
      }else{
        flushStmtCache( pDb );
      }
    }else if( *subCmd=='s' && strcmp(subCmd,"size")==0 ){
      if( objc!=4 ){
        Tcl_WrongNumArgs(interp, 2, objv, "size n");
        return TCL_ERROR;
      }else{
        if( TCL_ERROR==Tcl_GetIntFromObj(interp, objv[3], &n) ){
          Tcl_AppendResult( interp, "cannot convert \"", 
               Tcl_GetStringFromObj(objv[3],0), "\" to integer", 0);
          return TCL_ERROR;
        }else{
          if( n<0 ){
            flushStmtCache( pDb );
            n = 0;
          }else if( n>MAX_PREPARED_STMTS ){
            n = MAX_PREPARED_STMTS;
          }
          pDb->maxStmt = n;
        }
      }
    }else{
      Tcl_AppendResult( interp, "bad option \"", 
          Tcl_GetStringFromObj(objv[2],0), "\": must be flush or size", 0);
      return TCL_ERROR;
    }
    break;
  }

  /*     $db changes
  **
  ** Return the number of rows that were modified, inserted, or deleted by
  ** the most recent INSERT, UPDATE or DELETE statement, not including 
  ** any changes made by trigger programs.
  */
  case DB_CHANGES: {
    Tcl_Obj *pResult;
    if( objc!=2 ){
      Tcl_WrongNumArgs(interp, 2, objv, "");
      return TCL_ERROR;
    }
    pResult = Tcl_GetObjResult(interp);
    Tcl_SetIntObj(pResult, sqlite4_changes(pDb->db));
    break;
  }

  /*    $db close
  **
  ** Shutdown the database
  */
  case DB_CLOSE: {
    Tcl_DeleteCommand(interp, Tcl_GetStringFromObj(objv[0], 0));
    break;
  }

  /*
  **     $db collate NAME COMPARE-SCRIPT MKKEY-SCRIPT
  **
  ** Create a new SQL collation function called NAME.  Whenever
  ** that function is called, invoke SCRIPT to evaluate the function.
  */
  case DB_COLLATE: {
    SqlCollate *pCollate;
    char *zName;
    char *zCmp; int nCmp;
    char *zMkkey; int nMkkey;
    int nByte;

    if( objc!=5 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NAME CMP-SCRIPT MKKEY-SCRIPT");
      return TCL_ERROR;
    }
    zName = Tcl_GetStringFromObj(objv[2], 0);
    zCmp = Tcl_GetStringFromObj(objv[3], &nCmp);
    zMkkey = Tcl_GetStringFromObj(objv[4], &nMkkey);

    nByte = sizeof(SqlCollate) + nCmp + 1 + nMkkey + 1;
    pCollate = (SqlCollate*)Tcl_Alloc(nByte);
    pCollate->interp = interp;
    pCollate->pNext = pDb->pCollate;
    pCollate->zCmp = (char*)&pCollate[1];
    pCollate->zMkkey = &pCollate->zCmp[nCmp + 1];
    pDb->pCollate = pCollate;

    memcpy(pCollate->zCmp, zCmp, nCmp+1);
    memcpy(pCollate->zMkkey, zMkkey, nMkkey+1);
    rc = sqlite4_create_collation(
        pDb->db, zName, pCollate, tclSqlCollate, tclSqlMkkey, 0
    );
    if( rc!=SQLITE4_OK ){
      Tcl_SetResult(interp, (char *)sqlite4_errmsg(pDb->db), TCL_VOLATILE);
      return TCL_ERROR;
    }
    break;
  }

  /*
  **     $db collation_needed SCRIPT
  **
  ** Create a new SQL collation function called NAME.  Whenever
  ** that function is called, invoke SCRIPT to evaluate the function.
  */
  case DB_COLLATION_NEEDED: {
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SCRIPT");
      return TCL_ERROR;
    }
    if( pDb->pCollateNeeded ){
      Tcl_DecrRefCount(pDb->pCollateNeeded);
    }
    pDb->pCollateNeeded = Tcl_DuplicateObj(objv[2]);
    Tcl_IncrRefCount(pDb->pCollateNeeded);
    sqlite4_collation_needed(pDb->db, (void *)pDb, tclCollateNeeded, 0);
    break;
  }

  /*    $db complete SQL
  **
  ** Return TRUE if SQL is a complete SQL statement.  Return FALSE if
  ** additional lines of input are needed.  This is similar to the
  ** built-in "info complete" command of Tcl.
  */
  case DB_COMPLETE: {
#ifndef SQLITE4_OMIT_COMPLETE
    Tcl_Obj *pResult;
    int isComplete;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SQL");
      return TCL_ERROR;
    }
    isComplete = sqlite4_complete( Tcl_GetStringFromObj(objv[2], 0) );
    pResult = Tcl_GetObjResult(interp);
    Tcl_SetBooleanObj(pResult, isComplete);
#endif
    break;
  }

  /*    $db copy conflict-algorithm table filename ?SEPARATOR? ?NULLINDICATOR?
  **
  ** Copy data into table from filename, optionally using SEPARATOR
  ** as column separators.  If a column contains a null string, or the
  ** value of NULLINDICATOR, a NULL is inserted for the column.
  ** conflict-algorithm is one of the sqlite conflict algorithms:
  **    rollback, abort, fail, ignore, replace
  ** On success, return the number of lines processed, not necessarily same
  ** as 'db changes' due to conflict-algorithm selected.
  **
  ** This code is basically an implementation/enhancement of
  ** the sqlite4 shell.c ".import" command.
  **
  ** This command usage is equivalent to the sqlite2.x COPY statement,
  ** which imports file data into a table using the PostgreSQL COPY file format:
  **   $db copy $conflit_algo $table_name $filename \t \\N
  */
  case DB_COPY: {
    char *zTable;               /* Insert data into this table */
    char *zFile;                /* The file from which to extract data */
    char *zConflict;            /* The conflict algorithm to use */
    sqlite4_stmt *pStmt;        /* A statement */
    int nCol;                   /* Number of columns in the table */
    int nByte;                  /* Number of bytes in an SQL string */
    int i, j;                   /* Loop counters */
    int nSep;                   /* Number of bytes in zSep[] */
    int nNull;                  /* Number of bytes in zNull[] */
    char *zSql;                 /* An SQL statement */
    char *zLine;                /* A single line of input from the file */
    char **azCol;               /* zLine[] broken up into columns */
    char *zCommit;              /* How to commit changes */
    FILE *in;                   /* The input file */
    int lineno = 0;             /* Line number of input file */
    char zLineNum[80];          /* Line number print buffer */
    Tcl_Obj *pResult;           /* interp result */

    char *zSep;
    char *zNull;
    if( objc<5 || objc>7 ){
      Tcl_WrongNumArgs(interp, 2, objv, 
         "CONFLICT-ALGORITHM TABLE FILENAME ?SEPARATOR? ?NULLINDICATOR?");
      return TCL_ERROR;
    }
    if( objc>=6 ){
      zSep = Tcl_GetStringFromObj(objv[5], 0);
    }else{
      zSep = "\t";
    }
    if( objc>=7 ){
      zNull = Tcl_GetStringFromObj(objv[6], 0);
    }else{
      zNull = "";
    }
    zConflict = Tcl_GetStringFromObj(objv[2], 0);
    zTable = Tcl_GetStringFromObj(objv[3], 0);
    zFile = Tcl_GetStringFromObj(objv[4], 0);
    nSep = strlen30(zSep);
    nNull = strlen30(zNull);
    if( nSep==0 ){
      Tcl_AppendResult(interp,"Error: non-null separator required for copy",0);
      return TCL_ERROR;
    }
    if(strcmp(zConflict, "rollback") != 0 &&
       strcmp(zConflict, "abort"   ) != 0 &&
       strcmp(zConflict, "fail"    ) != 0 &&
       strcmp(zConflict, "ignore"  ) != 0 &&
       strcmp(zConflict, "replace" ) != 0 ) {
      Tcl_AppendResult(interp, "Error: \"", zConflict, 
            "\", conflict-algorithm must be one of: rollback, "
            "abort, fail, ignore, or replace", 0);
      return TCL_ERROR;
    }
    zSql = sqlite4_mprintf(0, "SELECT * FROM '%q'", zTable);
    if( zSql==0 ){
      Tcl_AppendResult(interp, "Error: no such table: ", zTable, 0);
      return TCL_ERROR;
    }
    nByte = strlen30(zSql);
    rc = sqlite4_prepare(pDb->db, zSql, -1, &pStmt, 0);
    sqlite4_free(0, zSql);
    if( rc ){
      Tcl_AppendResult(interp, "Error: ", sqlite4_errmsg(pDb->db), 0);
      nCol = 0;
    }else{
      nCol = sqlite4_column_count(pStmt);
    }
    sqlite4_finalize(pStmt);
    if( nCol==0 ) {
      return TCL_ERROR;
    }
    zSql = malloc( nByte + 50 + nCol*2 );
    if( zSql==0 ) {
      Tcl_AppendResult(interp, "Error: can't malloc()", 0);
      return TCL_ERROR;
    }
    j = sqlite4_snprintf(zSql, nByte+50, "INSERT OR %q INTO '%q' VALUES(?",
                         zConflict, zTable);
    for(i=1; i<nCol; i++){
      zSql[j++] = ',';
      zSql[j++] = '?';
    }
    zSql[j++] = ')';
    zSql[j] = 0;
    rc = sqlite4_prepare(pDb->db, zSql, -1, &pStmt, 0);
    free(zSql);
    if( rc ){
      Tcl_AppendResult(interp, "Error: ", sqlite4_errmsg(pDb->db), 0);
      sqlite4_finalize(pStmt);
      return TCL_ERROR;
    }
    in = fopen(zFile, "rb");
    if( in==0 ){
      Tcl_AppendResult(interp, "Error: cannot open file: ", zFile, NULL);
      sqlite4_finalize(pStmt);
      return TCL_ERROR;
    }
    azCol = malloc( sizeof(azCol[0])*(nCol+1) );
    if( azCol==0 ) {
      Tcl_AppendResult(interp, "Error: can't malloc()", 0);
      fclose(in);
      return TCL_ERROR;
    }
    (void)sqlite4_exec(pDb->db, "BEGIN", 0, 0);
    zCommit = "COMMIT";
    while( (zLine = local_getline(0, in))!=0 ){
      char *z;
      lineno++;
      azCol[0] = zLine;
      for(i=0, z=zLine; *z; z++){
        if( *z==zSep[0] && strncmp(z, zSep, nSep)==0 ){
          *z = 0;
          i++;
          if( i<nCol ){
            azCol[i] = &z[nSep];
            z += nSep-1;
          }
        }
      }
      if( i+1!=nCol ){
        char *zErr;
        int nErr = strlen30(zFile) + 200;
        zErr = malloc(nErr);
        if( zErr ){
          sqlite4_snprintf(zErr, nErr,
             "Error: %s line %d: expected %d columns of data but found %d",
             zFile, lineno, nCol, i+1);
          Tcl_AppendResult(interp, zErr, 0);
          free(zErr);
        }
        zCommit = "ROLLBACK";
        break;
      }
      for(i=0; i<nCol; i++){
        /* check for null data, if so, bind as null */
        if( (nNull>0 && strcmp(azCol[i], zNull)==0)
          || strlen30(azCol[i])==0 
        ){
          sqlite4_bind_null(pStmt, i+1);
        }else{
          sqlite4_bind_text(pStmt, i+1, azCol[i], -1, SQLITE4_STATIC, 0);
        }
      }
      sqlite4_step(pStmt);
      rc = sqlite4_reset(pStmt);
      free(zLine);
      if( rc!=SQLITE4_OK ){
        Tcl_AppendResult(interp,"Error: ", sqlite4_errmsg(pDb->db), 0);
        zCommit = "ROLLBACK";
        break;
      }
    }
    free(azCol);
    fclose(in);
    sqlite4_finalize(pStmt);
    (void)sqlite4_exec(pDb->db, zCommit, 0, 0);

    if( zCommit[0] == 'C' ){
      /* success, set result as number of lines processed */
      pResult = Tcl_GetObjResult(interp);
      Tcl_SetIntObj(pResult, lineno);
      rc = TCL_OK;
    }else{
      /* failure, append lineno where failed */
      sqlite4_snprintf(zLineNum, sizeof(zLineNum),"%d",lineno);
      Tcl_AppendResult(interp,", failed while processing line: ",zLineNum,0);
      rc = TCL_ERROR;
    }
    break;
  }

  /*
  **    $db enable_load_extension BOOLEAN
  **
  ** Turn the extension loading feature on or off.  It if off by
  ** default.
  */
  case DB_ENABLE_LOAD_EXTENSION: {
    Tcl_AppendResult(interp, "extension loading is turned off at compile-time",
                     0);
    return TCL_ERROR;
  }

  /*
  **    $db errorcode
  **
  ** Return the numeric error code that was returned by the most recent
  ** call to sqlite4_exec().
  */
  case DB_ERRORCODE: {
    Tcl_SetObjResult(interp, Tcl_NewIntObj(sqlite4_errcode(pDb->db)));
    break;
  }

  /*
  **    $db exists $sql
  **    $db onecolumn $sql
  **
  ** The onecolumn method is the equivalent of:
  **     lindex [$db eval $sql] 0
  */
  case DB_EXISTS: 
  case DB_ONECOLUMN: {
    DbEvalContext sEval;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SQL");
      return TCL_ERROR;
    }

    dbEvalInit(&sEval, pDb, objv[2], 0);
    rc = dbEvalStep(&sEval);
    if( choice==DB_ONECOLUMN ){
      if( rc==TCL_OK ){
        Tcl_SetObjResult(interp, dbEvalColumnValue(&sEval, 0));
      }else if( rc==TCL_BREAK ){
        Tcl_ResetResult(interp);
      }
    }else if( rc==TCL_BREAK || rc==TCL_OK ){
      Tcl_SetObjResult(interp, Tcl_NewBooleanObj(rc==TCL_OK));
    }
    dbEvalFinalize(&sEval);

    if( rc==TCL_BREAK ){
      rc = TCL_OK;
    }
    break;
  }
   
  /*
  **    $db eval $sql ?array? ?{  ...code... }?
  **
  ** The SQL statement in $sql is evaluated.  For each row, the values are
  ** placed in elements of the array named "array" and ...code... is executed.
  ** If "array" and "code" are omitted, then no callback is every invoked.
  ** If "array" is an empty string, then the values are placed in variables
  ** that have the same name as the fields extracted by the query.
  */
  case DB_EVAL: {
    if( objc<3 || objc>5 ){
      Tcl_WrongNumArgs(interp, 2, objv, "SQL ?ARRAY-NAME? ?SCRIPT?");
      return TCL_ERROR;
    }

    if( objc==3 ){
      DbEvalContext sEval;
      Tcl_Obj *pRet = Tcl_NewObj();
      Tcl_IncrRefCount(pRet);
      dbEvalInit(&sEval, pDb, objv[2], 0);
      while( TCL_OK==(rc = dbEvalStep(&sEval)) ){
        int i;
        int nCol;
        dbEvalRowInfo(&sEval, &nCol, 0);
        for(i=0; i<nCol; i++){
          Tcl_ListObjAppendElement(interp, pRet, dbEvalColumnValue(&sEval, i));
        }
      }
      dbEvalFinalize(&sEval);
      if( rc==TCL_BREAK ){
        Tcl_SetObjResult(interp, pRet);
        rc = TCL_OK;
      }
      Tcl_DecrRefCount(pRet);
    }else{
      ClientData cd[2];
      DbEvalContext *p;
      Tcl_Obj *pArray = 0;
      Tcl_Obj *pScript;

      if( objc==5 && *(char *)Tcl_GetString(objv[3]) ){
        pArray = objv[3];
      }
      pScript = objv[objc-1];
      Tcl_IncrRefCount(pScript);
      
      p = (DbEvalContext *)Tcl_Alloc(sizeof(DbEvalContext));
      dbEvalInit(p, pDb, objv[2], pArray);

      cd[0] = (void *)p;
      cd[1] = (void *)pScript;
      rc = DbEvalNextCmd(cd, interp, TCL_OK);
    }
    break;
  }

  /*
  **     $db function NAME [-argcount N] SCRIPT
  **
  ** Create a new SQL function called NAME.  Whenever that function is
  ** called, invoke SCRIPT to evaluate the function.
  */
  case DB_FUNCTION: {
    SqlFunc *pFunc;
    Tcl_Obj *pScript;
    char *zName;
    int nArg = -1;
    if( objc==6 ){
      const char *z = Tcl_GetString(objv[3]);
      int n = strlen30(z);
      if( n>2 && strncmp(z, "-argcount",n)==0 ){
        if( Tcl_GetIntFromObj(interp, objv[4], &nArg) ) return TCL_ERROR;
        if( nArg<0 ){
          Tcl_AppendResult(interp, "number of arguments must be non-negative",
                           (char*)0);
          return TCL_ERROR;
        }
      }
      pScript = objv[5];
    }else if( objc!=4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NAME [-argcount N] SCRIPT");
      return TCL_ERROR;
    }else{
      pScript = objv[3];
    }
    zName = Tcl_GetStringFromObj(objv[2], 0);
    pFunc = findSqlFunc(pDb, zName);
    if( pFunc==0 ) return TCL_ERROR;
    if( pFunc->pScript ){
      Tcl_DecrRefCount(pFunc->pScript);
    }
    pFunc->pScript = pScript;
    Tcl_IncrRefCount(pScript);
    pFunc->useEvalObjv = safeToUseEvalObjv(interp, pScript);
    rc = sqlite4_create_function(pDb->db, zName, nArg,
        pFunc, tclSqlFunc, 0, 0, 0);
    if( rc!=SQLITE4_OK ){
      rc = TCL_ERROR;
      Tcl_SetResult(interp, (char *)sqlite4_errmsg(pDb->db), TCL_VOLATILE);
    }
    break;
  }

  /*
  **     $db interrupt
  **
  ** Interrupt the execution of the inner-most SQL interpreter.  This
  ** causes the SQL statement to return an error of SQLITE4_INTERRUPT.
  */
  case DB_INTERRUPT: {
    sqlite4_interrupt(pDb->db);
    break;
  }

  /*
  **     $db nullvalue ?STRING?
  **
  ** Change text used when a NULL comes back from the database. If ?STRING?
  ** is not present, then the current string used for NULL is returned.
  ** If STRING is present, then STRING is returned.
  **
  */
  case DB_NULLVALUE: {
    if( objc!=2 && objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "NULLVALUE");
      return TCL_ERROR;
    }
    if( objc==3 ){
      int len;
      char *zNull = Tcl_GetStringFromObj(objv[2], &len);
      if( pDb->zNull ){
        Tcl_Free(pDb->zNull);
      }
      if( zNull && len>0 ){
        pDb->zNull = Tcl_Alloc( len + 1 );
        memcpy(pDb->zNull, zNull, len);
        pDb->zNull[len] = '\0';
      }else{
        pDb->zNull = 0;
      }
    }
    Tcl_SetObjResult(interp, dbTextToObj(pDb->zNull));
    break;
  }

  /*
  ** The DB_ONECOLUMN method is implemented together with DB_EXISTS.
  */


  /*    $db profile ?CALLBACK?
  **
  ** Make arrangements to invoke the CALLBACK routine after each SQL statement
  ** that has run.  The text of the SQL and the amount of elapse time are
  ** appended to CALLBACK before the script is run.
  */
  case DB_PROFILE: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( pDb->zProfile ){
        Tcl_AppendResult(interp, pDb->zProfile, 0);
      }
    }else{
      char *zProfile;
      int len;
      if( pDb->zProfile ){
        Tcl_Free(pDb->zProfile);
      }
      zProfile = Tcl_GetStringFromObj(objv[2], &len);
      if( zProfile && len>0 ){
        pDb->zProfile = Tcl_Alloc( len + 1 );
        memcpy(pDb->zProfile, zProfile, len+1);
      }else{
        pDb->zProfile = 0;
      }
#if !defined(SQLITE4_OMIT_TRACE) && !defined(SQLITE4_OMIT_FLOATING_POINT)
      if( pDb->zProfile ){
        pDb->interp = interp;
        sqlite4_profile(pDb->db, (void*)pDb, DbProfileHandler, 0);
      }else{
        sqlite4_profile(pDb->db, 0, 0, 0);
      }
#endif
    }
    break;
  }

  /*
  **     $db rekey KEY
  **
  ** Change the encryption key on the currently open database.
  */
  case DB_REKEY: {
#ifdef SQLITE4_HAS_CODEC
    int nKey;
    void *pKey;
#endif
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "KEY");
      return TCL_ERROR;
    }
#ifdef SQLITE4_HAS_CODEC
    pKey = Tcl_GetByteArrayFromObj(objv[2], &nKey);
    rc = sqlite4_rekey(pDb->db, pKey, nKey);
    if( rc ){
      Tcl_AppendResult(interp, sqlite4ErrStr(rc), 0);
      rc = TCL_ERROR;
    }
#endif
    break;
  }

  /*
  **     $db status (step|sort|autoindex)
  **
  ** Display SQLITE4_STMTSTATUS_FULLSCAN_STEP or 
  ** SQLITE4_STMTSTATUS_SORT for the most recent eval.
  */
  case DB_STATUS: {
    int v;
    const char *zOp;
    if( objc!=3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "(step|sort|autoindex)");
      return TCL_ERROR;
    }
    zOp = Tcl_GetString(objv[2]);
    if( strcmp(zOp, "step")==0 ){
      v = pDb->nStep;
    }else if( strcmp(zOp, "sort")==0 ){
      v = pDb->nSort;
    }else if( strcmp(zOp, "autoindex")==0 ){
      v = pDb->nIndex;
    }else{
      Tcl_AppendResult(interp, 
            "bad argument: should be autoindex, step, or sort", 
            (char*)0);
      return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(v));
    break;
  }
  
  /*
  **     $db total_changes
  **
  ** Return the number of rows that were modified, inserted, or deleted 
  ** since the database handle was created.
  */
  case DB_TOTAL_CHANGES: {
    Tcl_Obj *pResult;
    if( objc!=2 ){
      Tcl_WrongNumArgs(interp, 2, objv, "");
      return TCL_ERROR;
    }
    pResult = Tcl_GetObjResult(interp);
    Tcl_SetIntObj(pResult, sqlite4_total_changes(pDb->db));
    break;
  }

  /*    $db trace ?CALLBACK?
  **
  ** Make arrangements to invoke the CALLBACK routine for each SQL statement
  ** that is executed.  The text of the SQL is appended to CALLBACK before
  ** it is executed.
  */
  case DB_TRACE: {
    if( objc>3 ){
      Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
      return TCL_ERROR;
    }else if( objc==2 ){
      if( pDb->zTrace ){
        Tcl_AppendResult(interp, pDb->zTrace, 0);
      }
    }else{
      char *zTrace;
      int len;
      if( pDb->zTrace ){
        Tcl_Free(pDb->zTrace);
      }
      zTrace = Tcl_GetStringFromObj(objv[2], &len);
      if( zTrace && len>0 ){
        pDb->zTrace = Tcl_Alloc( len + 1 );
        memcpy(pDb->zTrace, zTrace, len+1);
      }else{
        pDb->zTrace = 0;
      }
#if !defined(SQLITE4_OMIT_TRACE) && !defined(SQLITE4_OMIT_FLOATING_POINT)
      if( pDb->zTrace ){
        pDb->interp = interp;
        sqlite4_trace(pDb->db, (void*)pDb, DbTraceHandler, 0);
      }else{
        sqlite4_trace(pDb->db, 0, 0, 0);
      }
#endif
    }
    break;
  }

  /*    $db transaction [-deferred|-immediate|-exclusive] SCRIPT
  **
  ** Start a new transaction (if we are not already in the midst of a
  ** transaction) and execute the TCL script SCRIPT.  After SCRIPT
  ** completes, either commit the transaction or roll it back if SCRIPT
  ** throws an exception.  Or if no new transation was started, do nothing.
  ** pass the exception on up the stack.
  **
  ** This command was inspired by Dave Thomas's talk on Ruby at the
  ** 2005 O'Reilly Open Source Convention (OSCON).
  */
  case DB_TRANSACTION: {
    Tcl_Obj *pScript;
    const char *zBegin = "SAVEPOINT _tcl_transaction";
    if( objc!=3 && objc!=4 ){
      Tcl_WrongNumArgs(interp, 2, objv, "[TYPE] SCRIPT");
      return TCL_ERROR;
    }

    if( pDb->nTransaction==0 && objc==4 ){
      static const char *TTYPE_strs[] = {
        "deferred",   "exclusive",  "immediate", 0
      };
      enum TTYPE_enum {
        TTYPE_DEFERRED, TTYPE_EXCLUSIVE, TTYPE_IMMEDIATE
      };
      int ttype;
      if( Tcl_GetIndexFromObj(interp, objv[2], TTYPE_strs, "transaction type",
                              0, &ttype) ){
        return TCL_ERROR;
      }
      switch( (enum TTYPE_enum)ttype ){
        case TTYPE_DEFERRED:    /* no-op */;                 break;
        case TTYPE_EXCLUSIVE:   zBegin = "BEGIN EXCLUSIVE";  break;
        case TTYPE_IMMEDIATE:   zBegin = "BEGIN IMMEDIATE";  break;
      }
    }
    pScript = objv[objc-1];

    /* Run the SQLite BEGIN command to open a transaction or savepoint. */
    pDb->disableAuth++;
    rc = sqlite4_exec(pDb->db, zBegin, 0, 0);
    pDb->disableAuth--;
    if( rc!=SQLITE4_OK ){
      Tcl_AppendResult(interp, sqlite4_errmsg(pDb->db), 0);
      return TCL_ERROR;
    }
    pDb->nTransaction++;

    /* If using NRE, schedule a callback to invoke the script pScript, then
    ** a second callback to commit (or rollback) the transaction or savepoint
    ** opened above. If not using NRE, evaluate the script directly, then
    ** call function DbTransPostCmd() to commit (or rollback) the transaction 
    ** or savepoint.  */
    if( DbUseNre() ){
      Tcl_NRAddCallback(interp, DbTransPostCmd, cd, 0, 0, 0);
      Tcl_NREvalObj(interp, pScript, 0);
    }else{
      rc = DbTransPostCmd(&cd, interp, Tcl_EvalObjEx(interp, pScript, 0));
    }
    break;
  }

  /*    $db version
  **
  ** Return the version string for this database.
  */
  case DB_VERSION: {
    Tcl_SetResult(interp, (char *)sqlite4_libversion(), TCL_STATIC);
    break;
  }


  } /* End of the SWITCH statement */
  return rc;
}

#if SQLITE4_TCL_NRE
/*
** Adaptor that provides an objCmd interface to the NRE-enabled
** interface implementation.
*/
static int DbObjCmdAdaptor(
  void *cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *const*objv
){
  return Tcl_NRCallObjProc(interp, DbObjCmd, cd, objc, objv);
}
#endif /* SQLITE4_TCL_NRE */

/*
**   sqlite4 DBNAME FILENAME ?-vfs VFSNAME? ?-key KEY? ?-readonly BOOLEAN?
**                           ?-create BOOLEAN? ?-nomutex BOOLEAN?
**
** This is the main Tcl command.  When the "sqlite" Tcl command is
** invoked, this routine runs to process that command.
**
** The first argument, DBNAME, is an arbitrary name for a new
** database connection.  This command creates a new command named
** DBNAME that is used to control that connection.  The database
** connection is deleted when the DBNAME command is deleted.
**
** The second argument is the name of the database file.
**
*/
static int DbMain(void *cd, Tcl_Interp *interp, int objc,Tcl_Obj *const*objv){
  SqliteDb *p;
  const char *zArg;
  char *zErrMsg;
  int i;
  const char *zFile;
  const char *zVfs = 0;
  int flags;
  Tcl_DString translatedFilename;
#ifdef SQLITE4_HAS_CODEC
  void *pKey = 0;
  int nKey = 0;
#endif

  flags = SQLITE4_OPEN_READWRITE | SQLITE4_OPEN_CREATE;
  if( objc==2 ){
    zArg = Tcl_GetStringFromObj(objv[1], 0);
    if( strcmp(zArg,"-version")==0 ){
      Tcl_AppendResult(interp,sqlite4_libversion(),0);
      return TCL_OK;
    }
    if( strcmp(zArg,"-has-codec")==0 ){
#ifdef SQLITE4_HAS_CODEC
      Tcl_AppendResult(interp,"1",0);
#else
      Tcl_AppendResult(interp,"0",0);
#endif
      return TCL_OK;
    }
  }
  for(i=3; i+1<objc; i+=2){
    zArg = Tcl_GetString(objv[i]);
    if( strcmp(zArg,"-key")==0 ){
#ifdef SQLITE4_HAS_CODEC
      pKey = Tcl_GetByteArrayFromObj(objv[i+1], &nKey);
#endif
    }else if( strcmp(zArg, "-vfs")==0 ){
      zVfs = Tcl_GetString(objv[i+1]);
    }else if( strcmp(zArg, "-readonly")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &b) ) return TCL_ERROR;
      if( b ){
        flags &= ~(SQLITE4_OPEN_READWRITE|SQLITE4_OPEN_CREATE);
        flags |= SQLITE4_OPEN_READONLY;
      }else{
        flags &= ~SQLITE4_OPEN_READONLY;
        flags |= SQLITE4_OPEN_READWRITE;
      }
    }else if( strcmp(zArg, "-create")==0 ){
      int b;
      if( Tcl_GetBooleanFromObj(interp, objv[i+1], &b) ) return TCL_ERROR;
      if( b && (flags & SQLITE4_OPEN_READONLY)==0 ){
        flags |= SQLITE4_OPEN_CREATE;
      }else{
        flags &= ~SQLITE4_OPEN_CREATE;
      }
    }else{
      Tcl_AppendResult(interp, "unknown option: ", zArg, (char*)0);
      return TCL_ERROR;
    }
  }
  if( objc<3 || (objc&1)!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, 
      "HANDLE FILENAME ?-vfs VFSNAME? ?-readonly BOOLEAN? ?-create BOOLEAN?"
      " ?-nomutex BOOLEAN? ?-fullmutex BOOLEAN? ?-uri BOOLEAN?"
#ifdef SQLITE4_HAS_CODEC
      " ?-key CODECKEY?"
#endif
    );
    return TCL_ERROR;
  }
  zErrMsg = 0;
  p = (SqliteDb*)Tcl_Alloc( sizeof(*p) );
  if( p==0 ){
    Tcl_SetResult(interp, "malloc failed", TCL_STATIC);
    return TCL_ERROR;
  }
  memset(p, 0, sizeof(*p));
  zFile = Tcl_GetStringFromObj(objv[2], 0);
  zFile = Tcl_TranslateFileName(interp, zFile, &translatedFilename);
  sqlite4_open(0, zFile, &p->db, 0);
  Tcl_DStringFree(&translatedFilename);
  if( SQLITE4_OK!=sqlite4_errcode(p->db) ){
    zErrMsg = sqlite4_mprintf(0, "%s", sqlite4_errmsg(p->db));
    sqlite4_close(p->db, 0);
    p->db = 0;
  }
#ifdef SQLITE4_TEST
  else{
    extern int sqlite4test_install_test_functions(sqlite4*);
    sqlite4test_install_test_functions(p->db);
  }
#endif
#ifdef SQLITE4_HAS_CODEC
  if( p->db ){
    sqlite4_key(p->db, pKey, nKey);
  }
#endif
  if( p->db==0 ){
    Tcl_SetResult(interp, zErrMsg, TCL_VOLATILE);
    Tcl_Free((char*)p);
    sqlite4_free(0, zErrMsg);
    return TCL_ERROR;
  }
  p->maxStmt = NUM_PREPARED_STMTS;
  p->interp = interp;
  zArg = Tcl_GetStringFromObj(objv[1], 0);
  if( DbUseNre() ){
    Tcl_NRCreateCommand(interp, zArg, DbObjCmdAdaptor, DbObjCmd,
                        (char*)p, DbDeleteCmd);
  }else{
    Tcl_CreateObjCommand(interp, zArg, DbObjCmd, (char*)p, DbDeleteCmd);
  }
  return TCL_OK;
}

/*
** Provide a dummy Tcl_InitStubs if we are using this as a static
** library.
*/
#ifndef USE_TCL_STUBS
# undef  Tcl_InitStubs
# define Tcl_InitStubs(a,b,c)
#endif

/*
** Make sure we have a PACKAGE_VERSION macro defined.  This will be
** defined automatically by the TEA makefile.  But other makefiles
** do not define it.
*/
#ifndef PACKAGE_VERSION
# define PACKAGE_VERSION SQLITE4_VERSION
#endif

/*
** Initialize this module.
**
** This Tcl module contains only a single new Tcl command named "sqlite".
** (Hence there is no namespace.  There is no point in using a namespace
** if the extension only supplies one new name!)  The "sqlite" command is
** used to open a new SQLite database.  See the DbMain() routine above
** for additional information.
**
** The EXTERN macros are required by TCL in order to work on windows.
*/
EXTERN int Sqlite3_Init(Tcl_Interp *interp){
  Tcl_InitStubs(interp, "8.4", 0);
  Tcl_CreateObjCommand(interp, "sqlite4", (Tcl_ObjCmdProc*)DbMain, 0, 0);
  Tcl_PkgProvide(interp, "sqlite4", PACKAGE_VERSION);

#ifndef SQLITE4_3_SUFFIX_ONLY
  /* The "sqlite" alias is undocumented.  It is here only to support
  ** legacy scripts.  All new scripts should use only the "sqlite4"
  ** command.
  */
  Tcl_CreateObjCommand(interp, "sqlite", (Tcl_ObjCmdProc*)DbMain, 0, 0);
#endif

  return TCL_OK;
}
EXTERN int Tclsqlite4_Init(Tcl_Interp *interp){ return Sqlite3_Init(interp); }
EXTERN int Sqlite3_SafeInit(Tcl_Interp *interp){ return TCL_OK; }
EXTERN int Tclsqlite4_SafeInit(Tcl_Interp *interp){ return TCL_OK; }
EXTERN int Sqlite3_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Tclsqlite4_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Sqlite3_SafeUnload(Tcl_Interp *interp, int flags){ return TCL_OK; }
EXTERN int Tclsqlite4_SafeUnload(Tcl_Interp *interp, int flags){ return TCL_OK;}


#ifndef SQLITE4_3_SUFFIX_ONLY
int Sqlite_Init(Tcl_Interp *interp){ return Sqlite3_Init(interp); }
int Tclsqlite_Init(Tcl_Interp *interp){ return Sqlite3_Init(interp); }
int Sqlite_SafeInit(Tcl_Interp *interp){ return TCL_OK; }
int Tclsqlite_SafeInit(Tcl_Interp *interp){ return TCL_OK; }
int Sqlite_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
int Tclsqlite_Unload(Tcl_Interp *interp, int flags){ return TCL_OK; }
int Sqlite_SafeUnload(Tcl_Interp *interp, int flags){ return TCL_OK; }
int Tclsqlite_SafeUnload(Tcl_Interp *interp, int flags){ return TCL_OK;}
#endif

#ifdef TCLSH
/*****************************************************************************
** All of the code that follows is used to build standalone TCL interpreters
** that are statically linked with SQLite.  Enable these by compiling
** with -DTCLSH=n where n can be 1 or 2.  An n of 1 generates a standard
** tclsh but with SQLite built in.  An n of 2 generates the SQLite space
** analysis program.
*/

#if defined(SQLITE4_TEST) || defined(SQLITE4_TCLMD5)
/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

/*
 * If compiled on a machine that doesn't have a 32-bit integer,
 * you just set "uint32" to the appropriate datatype for an
 * unsigned 32-bit integer.  For example:
 *
 *       cc -Duint32='unsigned long' md5.c
 *
 */
#ifndef uint32
#  define uint32 unsigned int
#endif

struct MD5Context {
  int isInit;
  uint32 buf[4];
  uint32 bits[2];
  unsigned char in[64];
};
typedef struct MD5Context MD5Context;

/*
 * Note: this code is harmless on little-endian machines.
 */
static void byteReverse (unsigned char *buf, unsigned longs){
        uint32 t;
        do {
                t = (uint32)((unsigned)buf[3]<<8 | buf[2]) << 16 |
                            ((unsigned)buf[1]<<8 | buf[0]);
                *(uint32 *)buf = t;
                buf += 4;
        } while (--longs);
}
/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
        ( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void MD5Transform(uint32 buf[4], const uint32 in[16]){
        register uint32 a, b, c, d;

        a = buf[0];
        b = buf[1];
        c = buf[2];
        d = buf[3];

        MD5STEP(F1, a, b, c, d, in[ 0]+0xd76aa478,  7);
        MD5STEP(F1, d, a, b, c, in[ 1]+0xe8c7b756, 12);
        MD5STEP(F1, c, d, a, b, in[ 2]+0x242070db, 17);
        MD5STEP(F1, b, c, d, a, in[ 3]+0xc1bdceee, 22);
        MD5STEP(F1, a, b, c, d, in[ 4]+0xf57c0faf,  7);
        MD5STEP(F1, d, a, b, c, in[ 5]+0x4787c62a, 12);
        MD5STEP(F1, c, d, a, b, in[ 6]+0xa8304613, 17);
        MD5STEP(F1, b, c, d, a, in[ 7]+0xfd469501, 22);
        MD5STEP(F1, a, b, c, d, in[ 8]+0x698098d8,  7);
        MD5STEP(F1, d, a, b, c, in[ 9]+0x8b44f7af, 12);
        MD5STEP(F1, c, d, a, b, in[10]+0xffff5bb1, 17);
        MD5STEP(F1, b, c, d, a, in[11]+0x895cd7be, 22);
        MD5STEP(F1, a, b, c, d, in[12]+0x6b901122,  7);
        MD5STEP(F1, d, a, b, c, in[13]+0xfd987193, 12);
        MD5STEP(F1, c, d, a, b, in[14]+0xa679438e, 17);
        MD5STEP(F1, b, c, d, a, in[15]+0x49b40821, 22);

        MD5STEP(F2, a, b, c, d, in[ 1]+0xf61e2562,  5);
        MD5STEP(F2, d, a, b, c, in[ 6]+0xc040b340,  9);
        MD5STEP(F2, c, d, a, b, in[11]+0x265e5a51, 14);
        MD5STEP(F2, b, c, d, a, in[ 0]+0xe9b6c7aa, 20);
        MD5STEP(F2, a, b, c, d, in[ 5]+0xd62f105d,  5);
        MD5STEP(F2, d, a, b, c, in[10]+0x02441453,  9);
        MD5STEP(F2, c, d, a, b, in[15]+0xd8a1e681, 14);
        MD5STEP(F2, b, c, d, a, in[ 4]+0xe7d3fbc8, 20);
        MD5STEP(F2, a, b, c, d, in[ 9]+0x21e1cde6,  5);
        MD5STEP(F2, d, a, b, c, in[14]+0xc33707d6,  9);
        MD5STEP(F2, c, d, a, b, in[ 3]+0xf4d50d87, 14);
        MD5STEP(F2, b, c, d, a, in[ 8]+0x455a14ed, 20);
        MD5STEP(F2, a, b, c, d, in[13]+0xa9e3e905,  5);
        MD5STEP(F2, d, a, b, c, in[ 2]+0xfcefa3f8,  9);
        MD5STEP(F2, c, d, a, b, in[ 7]+0x676f02d9, 14);
        MD5STEP(F2, b, c, d, a, in[12]+0x8d2a4c8a, 20);

        MD5STEP(F3, a, b, c, d, in[ 5]+0xfffa3942,  4);
        MD5STEP(F3, d, a, b, c, in[ 8]+0x8771f681, 11);
        MD5STEP(F3, c, d, a, b, in[11]+0x6d9d6122, 16);
        MD5STEP(F3, b, c, d, a, in[14]+0xfde5380c, 23);
        MD5STEP(F3, a, b, c, d, in[ 1]+0xa4beea44,  4);
        MD5STEP(F3, d, a, b, c, in[ 4]+0x4bdecfa9, 11);
        MD5STEP(F3, c, d, a, b, in[ 7]+0xf6bb4b60, 16);
        MD5STEP(F3, b, c, d, a, in[10]+0xbebfbc70, 23);
        MD5STEP(F3, a, b, c, d, in[13]+0x289b7ec6,  4);
        MD5STEP(F3, d, a, b, c, in[ 0]+0xeaa127fa, 11);
        MD5STEP(F3, c, d, a, b, in[ 3]+0xd4ef3085, 16);
        MD5STEP(F3, b, c, d, a, in[ 6]+0x04881d05, 23);
        MD5STEP(F3, a, b, c, d, in[ 9]+0xd9d4d039,  4);
        MD5STEP(F3, d, a, b, c, in[12]+0xe6db99e5, 11);
        MD5STEP(F3, c, d, a, b, in[15]+0x1fa27cf8, 16);
        MD5STEP(F3, b, c, d, a, in[ 2]+0xc4ac5665, 23);

        MD5STEP(F4, a, b, c, d, in[ 0]+0xf4292244,  6);
        MD5STEP(F4, d, a, b, c, in[ 7]+0x432aff97, 10);
        MD5STEP(F4, c, d, a, b, in[14]+0xab9423a7, 15);
        MD5STEP(F4, b, c, d, a, in[ 5]+0xfc93a039, 21);
        MD5STEP(F4, a, b, c, d, in[12]+0x655b59c3,  6);
        MD5STEP(F4, d, a, b, c, in[ 3]+0x8f0ccc92, 10);
        MD5STEP(F4, c, d, a, b, in[10]+0xffeff47d, 15);
        MD5STEP(F4, b, c, d, a, in[ 1]+0x85845dd1, 21);
        MD5STEP(F4, a, b, c, d, in[ 8]+0x6fa87e4f,  6);
        MD5STEP(F4, d, a, b, c, in[15]+0xfe2ce6e0, 10);
        MD5STEP(F4, c, d, a, b, in[ 6]+0xa3014314, 15);
        MD5STEP(F4, b, c, d, a, in[13]+0x4e0811a1, 21);
        MD5STEP(F4, a, b, c, d, in[ 4]+0xf7537e82,  6);
        MD5STEP(F4, d, a, b, c, in[11]+0xbd3af235, 10);
        MD5STEP(F4, c, d, a, b, in[ 2]+0x2ad7d2bb, 15);
        MD5STEP(F4, b, c, d, a, in[ 9]+0xeb86d391, 21);

        buf[0] += a;
        buf[1] += b;
        buf[2] += c;
        buf[3] += d;
}

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
static void MD5Init(MD5Context *ctx){
        ctx->isInit = 1;
        ctx->buf[0] = 0x67452301;
        ctx->buf[1] = 0xefcdab89;
        ctx->buf[2] = 0x98badcfe;
        ctx->buf[3] = 0x10325476;
        ctx->bits[0] = 0;
        ctx->bits[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
static 
void MD5Update(MD5Context *ctx, const unsigned char *buf, unsigned int len){
        uint32 t;

        /* Update bitcount */

        t = ctx->bits[0];
        if ((ctx->bits[0] = t + ((uint32)len << 3)) < t)
                ctx->bits[1]++; /* Carry from low to high */
        ctx->bits[1] += len >> 29;

        t = (t >> 3) & 0x3f;    /* Bytes already in shsInfo->data */

        /* Handle any leading odd-sized chunks */

        if ( t ) {
                unsigned char *p = (unsigned char *)ctx->in + t;

                t = 64-t;
                if (len < t) {
                        memcpy(p, buf, len);
                        return;
                }
                memcpy(p, buf, t);
                byteReverse(ctx->in, 16);
                MD5Transform(ctx->buf, (uint32 *)ctx->in);
                buf += t;
                len -= t;
        }

        /* Process data in 64-byte chunks */

        while (len >= 64) {
                memcpy(ctx->in, buf, 64);
                byteReverse(ctx->in, 16);
                MD5Transform(ctx->buf, (uint32 *)ctx->in);
                buf += 64;
                len -= 64;
        }

        /* Handle any remaining bytes of data. */

        memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern 
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
static void MD5Final(unsigned char digest[16], MD5Context *ctx){
        unsigned count;
        unsigned char *p;

        /* Compute number of bytes mod 64 */
        count = (ctx->bits[0] >> 3) & 0x3F;

        /* Set the first char of padding to 0x80.  This is safe since there is
           always at least one byte free */
        p = ctx->in + count;
        *p++ = 0x80;

        /* Bytes of padding needed to make 64 bytes */
        count = 64 - 1 - count;

        /* Pad out to 56 mod 64 */
        if (count < 8) {
                /* Two lots of padding:  Pad the first block to 64 bytes */
                memset(p, 0, count);
                byteReverse(ctx->in, 16);
                MD5Transform(ctx->buf, (uint32 *)ctx->in);

                /* Now fill the next block with 56 bytes */
                memset(ctx->in, 0, 56);
        } else {
                /* Pad block to 56 bytes */
                memset(p, 0, count-8);
        }
        byteReverse(ctx->in, 14);

        /* Append length in bits and transform */
        ((uint32 *)ctx->in)[ 14 ] = ctx->bits[0];
        ((uint32 *)ctx->in)[ 15 ] = ctx->bits[1];

        MD5Transform(ctx->buf, (uint32 *)ctx->in);
        byteReverse((unsigned char *)ctx->buf, 4);
        memcpy(digest, ctx->buf, 16);
        memset(ctx, 0, sizeof(ctx));    /* In case it is sensitive */
}

/*
** Convert a 128-bit MD5 digest into a 32-digit base-16 number.
*/
static void MD5DigestToBase16(unsigned char *digest, char *zBuf){
  static char const zEncode[] = "0123456789abcdef";
  int i, j;

  for(j=i=0; i<16; i++){
    int a = digest[i];
    zBuf[j++] = zEncode[(a>>4)&0xf];
    zBuf[j++] = zEncode[a & 0xf];
  }
  zBuf[j] = 0;
}


/*
** Convert a 128-bit MD5 digest into sequency of eight 5-digit integers
** each representing 16 bits of the digest and separated from each
** other by a "-" character.
*/
static void MD5DigestToBase10x8(unsigned char digest[16], char zDigest[50]){
  int i, j;
  unsigned int x;
  for(i=j=0; i<16; i+=2){
    x = digest[i]*256 + digest[i+1];
    if( i>0 ) zDigest[j++] = '-';
    sprintf(&zDigest[j], "%05u", x);
    j += 5;
  }
  zDigest[j] = 0;
}

/*
** A TCL command for md5.  The argument is the text to be hashed.  The
** Result is the hash in base64.  
*/
static int md5_cmd(void*cd, Tcl_Interp *interp, int argc, const char **argv){
  MD5Context ctx;
  unsigned char digest[16];
  char zBuf[50];
  void (*converter)(unsigned char*, char*);

  if( argc!=2 ){
    Tcl_AppendResult(interp,"wrong # args: should be \"", argv[0], 
        " TEXT\"", 0);
    return TCL_ERROR;
  }
  MD5Init(&ctx);
  MD5Update(&ctx, (unsigned char*)argv[1], (unsigned)strlen(argv[1]));
  MD5Final(digest, &ctx);
  converter = (void(*)(unsigned char*,char*))cd;
  converter(digest, zBuf);
  Tcl_AppendResult(interp, zBuf, (char*)0);
  return TCL_OK;
}

/*
** A TCL command to take the md5 hash of a file.  The argument is the
** name of the file.
*/
static int md5file_cmd(void*cd, Tcl_Interp*interp, int argc, const char **argv){
  FILE *in;
  MD5Context ctx;
  void (*converter)(unsigned char*, char*);
  unsigned char digest[16];
  char zBuf[10240];

  if( argc!=2 ){
    Tcl_AppendResult(interp,"wrong # args: should be \"", argv[0], 
        " FILENAME\"", 0);
    return TCL_ERROR;
  }
  in = fopen(argv[1],"rb");
  if( in==0 ){
    Tcl_AppendResult(interp,"unable to open file \"", argv[1], 
         "\" for reading", 0);
    return TCL_ERROR;
  }
  MD5Init(&ctx);
  for(;;){
    int n;
    n = fread(zBuf, 1, sizeof(zBuf), in);
    if( n<=0 ) break;
    MD5Update(&ctx, (unsigned char*)zBuf, (unsigned)n);
  }
  fclose(in);
  MD5Final(digest, &ctx);
  converter = (void(*)(unsigned char*,char*))cd;
  converter(digest, zBuf);
  Tcl_AppendResult(interp, zBuf, (char*)0);
  return TCL_OK;
}

/*
** Register the four new TCL commands for generating MD5 checksums
** with the TCL interpreter.
*/
int Md5_Init(Tcl_Interp *interp){
  Tcl_CreateCommand(interp, "md5", (Tcl_CmdProc*)md5_cmd,
                    MD5DigestToBase16, 0);
  Tcl_CreateCommand(interp, "md5-10x8", (Tcl_CmdProc*)md5_cmd,
                    MD5DigestToBase10x8, 0);
  Tcl_CreateCommand(interp, "md5file", (Tcl_CmdProc*)md5file_cmd,
                    MD5DigestToBase16, 0);
  Tcl_CreateCommand(interp, "md5file-10x8", (Tcl_CmdProc*)md5file_cmd,
                    MD5DigestToBase10x8, 0);
  return TCL_OK;
}
#endif /* defined(SQLITE4_TEST) || defined(SQLITE4_TCLMD5) */

#if defined(SQLITE4_TEST)
/*
** During testing, the special md5sum() aggregate function is available.
** inside SQLite.  The following routines implement that function.
*/
static void md5step(sqlite4_context *context, int argc, sqlite4_value **argv){
  MD5Context *p;
  int i;
  if( argc<1 ) return;
  p = sqlite4_aggregate_context(context, sizeof(*p));
  if( p==0 ) return;
  if( !p->isInit ){
    MD5Init(p);
  }
  for(i=0; i<argc; i++){
    const char *zData = (char*)sqlite4_value_text(argv[i], 0);
    if( zData ){
      MD5Update(p, (unsigned char*)zData, strlen(zData));
    }
  }
}
static void md5finalize(sqlite4_context *context){
  MD5Context *p;
  unsigned char digest[16];
  char zBuf[33];
  p = sqlite4_aggregate_context(context, sizeof(*p));
  MD5Final(digest,p);
  MD5DigestToBase16(digest, zBuf);
  sqlite4_result_text(context, zBuf, -1, SQLITE4_TRANSIENT, 0);
}
int Md5_Register(sqlite4 *db){
  int rc = sqlite4_create_function(db, "md5sum", -1, 0, 0, 
                                 md5step, md5finalize, 0);
  sqlite4_overload_function(db, "md5sum", -1);  /* To exercise this API */
  return rc;
}
#endif /* defined(SQLITE4_TEST) */


/*
** If the macro TCLSH is one, then put in code this for the
** "main" routine that will initialize Tcl and take input from
** standard input, or if a file is named on the command line
** the TCL interpreter reads and evaluates that file.
*/
#if TCLSH==1
static const char *tclsh_main_loop(void){
  static const char zMainloop[] =
    "set line {}\n"
    "while {![eof stdin]} {\n"
      "if {$line!=\"\"} {\n"
        "puts -nonewline \"> \"\n"
      "} else {\n"
        "puts -nonewline \"% \"\n"
      "}\n"
      "flush stdout\n"
      "append line [gets stdin]\n"
      "if {[info complete $line]} {\n"
        "if {[catch {uplevel #0 $line} result]} {\n"
          "puts stderr \"Error: $result\"\n"
        "} elseif {$result!=\"\"} {\n"
          "puts $result\n"
        "}\n"
        "set line {}\n"
      "} else {\n"
        "append line \\n\n"
      "}\n"
    "}\n"
  ;
  return zMainloop;
}
#endif
#if TCLSH==2
static const char *tclsh_main_loop(void);
#endif

#ifdef SQLITE4_TEST
static void init_all(Tcl_Interp *);
static int init_all_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){

  Tcl_Interp *slave;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SLAVE");
    return TCL_ERROR;
  }

  slave = Tcl_GetSlave(interp, Tcl_GetString(objv[1]));
  if( !slave ){
    return TCL_ERROR;
  }

  init_all(slave);
  return TCL_OK;
}


/*
** Tclcmd: db_use_legacy_prepare DB BOOLEAN
**
**   The first argument to this command must be a database command created by
**   [sqlite4]. If the second argument is true, then the handle is configured
**   to use the sqlite4_prepare_v2() function to prepare statements. If it
**   is false, sqlite4_prepare().
*/
static int db_use_legacy_prepare_cmd(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_CmdInfo cmdInfo;
  SqliteDb *pDb;
  int bPrepare;

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB BOOLEAN");
    return TCL_ERROR;
  }

  if( !Tcl_GetCommandInfo(interp, Tcl_GetString(objv[1]), &cmdInfo) ){
    Tcl_AppendResult(interp, "no such db: ", Tcl_GetString(objv[1]), (char*)0);
    return TCL_ERROR;
  }
  pDb = (SqliteDb*)cmdInfo.objClientData;
  if( Tcl_GetBooleanFromObj(interp, objv[2], &bPrepare) ){
    return TCL_ERROR;
  }

  Tcl_ResetResult(interp);
  return TCL_OK;
}
#endif

/*
** Configure the interpreter passed as the first argument to have access
** to the commands and linked variables that make up:
**
**   * the [sqlite4] extension itself, 
**
**   * If SQLITE4_TCLMD5 or SQLITE4_TEST is defined, the Md5 commands, and
**
**   * If SQLITE4_TEST is set, the various test interfaces used by the Tcl
**     test suite.
*/
static void init_all(Tcl_Interp *interp){
  Sqlite3_Init(interp);

#if defined(SQLITE4_TEST) || defined(SQLITE4_TCLMD5)
  Md5_Init(interp);
#endif

#ifdef SQLITE4_TEST
  {
    extern int Sqliteconfig_Init(Tcl_Interp*);
    extern int Sqlitetest1_Init(Tcl_Interp*);
    extern int Sqlitetest4_Init(Tcl_Interp*);
    extern int Sqlitetest5_Init(Tcl_Interp*);
    extern int Sqlitetest9_Init(Tcl_Interp*);
    extern int Sqlitetest_func_Init(Tcl_Interp*);
    extern int Sqlitetest_hexio_Init(Tcl_Interp*);
    extern int Sqlitetest_malloc_Init(Tcl_Interp*);
    extern int Sqlitetest_mutex_Init(Tcl_Interp*);
    extern int Sqlitetestschema_Init(Tcl_Interp*);
    extern int Sqlitetestsse_Init(Tcl_Interp*);
    extern int Sqlitetesttclvar_Init(Tcl_Interp*);
    extern int SqlitetestThread_Init(Tcl_Interp*);
    extern int Sqlitetestintarray_Init(Tcl_Interp*);
    extern int Sqlitetestrtree_Init(Tcl_Interp*);
    extern int Sqlitequota_Init(Tcl_Interp*);
    extern int SqliteSuperlock_Init(Tcl_Interp*);
    extern int SqlitetestSyscall_Init(Tcl_Interp*);
    extern int Sqliteteststorage_Init(Tcl_Interp*);
    extern int Sqliteteststorage2_Init(Tcl_Interp*);
    extern int SqlitetestLsm_Init(Tcl_Interp*);

    extern void sqlite4TestInit(Tcl_Interp*);

    Sqliteconfig_Init(interp);
    Sqlitetest1_Init(interp);
    Sqlitetest4_Init(interp);
    Sqlitetest5_Init(interp);
    Sqlitetest9_Init(interp);
    Sqlitetest_hexio_Init(interp);
    Sqlitetest_malloc_Init(interp);
    Sqlitetest_mutex_Init(interp);
    SqlitetestThread_Init(interp);
    Sqliteteststorage_Init(interp);
    Sqliteteststorage2_Init(interp);
    SqlitetestLsm_Init(interp);
    sqlite4TestInit(interp);

    Tcl_CreateObjCommand(
        interp, "load_testfixture_extensions", init_all_cmd, 0, 0
    );
    Tcl_CreateObjCommand(
        interp, "db_use_legacy_prepare", db_use_legacy_prepare_cmd, 0, 0
    );

#ifdef SQLITE4_SSE
    Sqlitetestsse_Init(interp);
#endif
  }
#endif
}

#define TCLSH_MAIN main   /* Needed to fake out mktclapp */
int TCLSH_MAIN(int argc, char **argv){
  Tcl_Interp *interp;
  
  /* Call sqlite4_shutdown() once before doing anything else. This is to
  ** test that sqlite4_shutdown() can be safely called by a process before
  ** sqlite4_initialize() is. */
  sqlite4_shutdown(0);

  /*Tcl_FindExecutable(argv[0]);*/
  interp = Tcl_CreateInterp();

#if TCLSH==2
  sqlite4_env_config(0, SQLITE4_ENVCONFIG_SINGLETHREAD);
#endif

  init_all(interp);
  if( argc>=2 ){
    int i;
    char zArgc[32];
    sqlite4_snprintf(zArgc, sizeof(zArgc), "%d", argc-(3-TCLSH));
    Tcl_SetVar(interp,"argc", zArgc, TCL_GLOBAL_ONLY);
    Tcl_SetVar(interp,"argv0",argv[1],TCL_GLOBAL_ONLY);
    Tcl_SetVar(interp,"argv", "", TCL_GLOBAL_ONLY);
    for(i=3-TCLSH; i<argc; i++){
      Tcl_SetVar(interp, "argv", argv[i],
          TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT | TCL_APPEND_VALUE);
    }
    if( TCLSH==1 && Tcl_EvalFile(interp, argv[1])!=TCL_OK ){
      const char *zInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
      if( zInfo==0 ) zInfo = Tcl_GetStringResult(interp);
      fprintf(stderr,"%s: %s\n", *argv, zInfo);
      Tcl_DeleteInterp( interp );
      return 1;
    }
  }
  if( TCLSH==2 || argc<=1 ){
    Tcl_GlobalEval(interp, tclsh_main_loop());
  }
  Tcl_DeleteInterp( interp );
  return 0;
}
#endif /* TCLSH */
