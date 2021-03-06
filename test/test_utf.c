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
** Code for testing the utf.c module in SQLite.  This code
** is not included in the SQLite library.  It is used for automated
** testing of the SQLite library. Specifically, the code in this file
** is used for testing the SQLite routines for converting between
** the various supported unicode encodings.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"
#include "tcl.h"
#include <stdlib.h>
#include <string.h>

/*
** The first argument is a TCL UTF-8 string. Return the byte array
** object with the encoded representation of the string, including
** the NULL terminator.
*/
static int binarize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int len;
  int bNoterm = 0;
  char *bytes;
  Tcl_Obj *pRet;

  if( objc!=2 && objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "?-noterm? STR");
    return TCL_ERROR;
  }
  if( objc==3 ){
    bNoterm = 1;
  }

  bytes = Tcl_GetStringFromObj(objv[objc-1], &len);
  pRet = Tcl_NewByteArrayObj((u8*)bytes, len+1-bNoterm);
  Tcl_SetObjResult(interp, pRet);
  return TCL_OK;
}

/*
** Usage: test_value_overhead <repeat-count> <do-calls>.
**
** This routine is used to test the overhead of calls to
** sqlite4_value_text(), on a value that contains a UTF-8 string. The idea
** is to figure out whether or not it is a problem to use sqlite4_value
** structures with collation sequence functions.
**
** If <do-calls> is 0, then the calls to sqlite4_value_text() are not
** actually made.
*/
static int test_value_overhead(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int do_calls;
  int repeat_count;
  int i;
  Mem val;
  const char *zVal;

  if( objc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"",
        Tcl_GetStringFromObj(objv[0], 0), " <repeat-count> <do-calls>", 0);
    return TCL_ERROR;
  }

  if( Tcl_GetIntFromObj(interp, objv[1], &repeat_count) ) return TCL_ERROR;
  if( Tcl_GetIntFromObj(interp, objv[2], &do_calls) ) return TCL_ERROR;

  val.flags = MEM_Str|MEM_Term|MEM_Static;
  val.z = "hello world";
  val.type = SQLITE4_TEXT;
  val.enc = SQLITE4_UTF8;

  for(i=0; i<repeat_count; i++){
    if( do_calls ){
      zVal = sqlite4_value_text(&val, 0);
      UNUSED_PARAMETER(zVal);
    }
  }

  return TCL_OK;
}

static u8 name_to_enc(Tcl_Interp *interp, Tcl_Obj *pObj){
  struct EncName {
    char *zName;
    u8 enc;
  } encnames[] = {
    { "UTF8", SQLITE4_UTF8 },
    { "UTF16LE", SQLITE4_UTF16LE },
    { "UTF16BE", SQLITE4_UTF16BE },
    { "UTF16", SQLITE4_UTF16 },
    { 0, 0 }
  };
  struct EncName *pEnc;
  char *z = Tcl_GetString(pObj);
  for(pEnc=&encnames[0]; pEnc->zName; pEnc++){
    if( 0==sqlite4_stricmp(z, pEnc->zName) ){
      break;
    }
  }
  if( !pEnc->enc ){
    Tcl_AppendResult(interp, "No such encoding: ", z, 0);
  }
  if( pEnc->enc==SQLITE4_UTF16 ){
    return SQLITE4_UTF16NATIVE;
  }
  return pEnc->enc;
}

static void freeStr(void *pEnv, void *pStr){
  sqlite4_free((sqlite4_env*)pEnv, pStr);
}

/*
** Usage:   sqlite4_translate <string/blob> <translation>
*/
static int test_sqlite4_translate(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct Translation {
    const char *zTrans;
    int eTrans;
    int isUtf8;
  } aTrans[] = {
    { "utf8_utf16",   SQLITE4_TRANSLATE_UTF8_UTF16,   1 },
    { "utf8_utf16le", SQLITE4_TRANSLATE_UTF8_UTF16LE, 1 },
    { "utf8_utf16be", SQLITE4_TRANSLATE_UTF8_UTF16BE, 1 },
    { "utf16_utf8",   SQLITE4_TRANSLATE_UTF16_UTF8,   0 },
    { "utf16le_utf8", SQLITE4_TRANSLATE_UTF16LE_UTF8, 0 },
    { "utf16be_utf8", SQLITE4_TRANSLATE_UTF16BE_UTF8, 0 },
    { 0, 0, 0 }
  };

  int rc;                         /* Return code */
  int iOpt;                       /* Index into aTrans[] array */

  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "VALUE TRANSLATION");
    rc = TCL_ERROR;
  }else{
    rc = Tcl_GetIndexFromObjStruct(
        interp, objv[2], aTrans, sizeof(aTrans[0]), "translation", 0, &iOpt
    );
  }

  if( rc==TCL_OK ){
    sqlite4_buffer buf;
    void *p;
    int n;
    int isUtf8 = aTrans[iOpt].isUtf8;
    int eTrans = aTrans[iOpt].eTrans;

    if( isUtf8 ){
      p = Tcl_GetString(objv[1]);
      n = strlen((char *)p);
    }else{
      p = Tcl_GetByteArrayFromObj(objv[1], &n);
    }

    sqlite4_buffer_init(&buf, 0);
    sqlite4_translate(&buf, p, n, eTrans);
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(buf.p, buf.n));
    sqlite4_buffer_clear(&buf);
  }

  return rc;
}

/*
** Usage:   test_translate <string/blob> <from enc> <to enc> ?<transient>?
**
*/
static int test_translate(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  u8 enc_from;
  u8 enc_to;
  sqlite4_value *pVal;

  char *z;
  int len;
  void (*xDel)(void*,void*) = SQLITE4_STATIC;

  if( objc!=4 && objc!=5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"",
        Tcl_GetStringFromObj(objv[0], 0), 
        " <string/blob> <from enc> <to enc>", 0
    );
    return TCL_ERROR;
  }
  if( objc==5 ){
    xDel = freeStr;
  }

  enc_from = name_to_enc(interp, objv[2]);
  if( !enc_from ) return TCL_ERROR;
  enc_to = name_to_enc(interp, objv[3]);
  if( !enc_to ) return TCL_ERROR;

  pVal = sqlite4ValueNew(0);

  if( enc_from==SQLITE4_UTF8 ){
    z = Tcl_GetString(objv[1]);
    if( objc==5 ){
      z = sqlite4_mprintf(0, "%s", z);
    }
    sqlite4ValueSetStr(pVal, -1, z, enc_from, xDel, 0);
  }else{
    z = (char*)Tcl_GetByteArrayFromObj(objv[1], &len);
    if( objc==5 ){
      char *zTmp = z;
      z = sqlite4_malloc(0, len);
      memcpy(z, zTmp, len);
    }
    sqlite4ValueSetStr(pVal, -1, z, enc_from, xDel, 0);
  }

  z = (char *)sqlite4ValueText(pVal, enc_to);
  len = sqlite4ValueBytes(pVal, enc_to) + (enc_to==SQLITE4_UTF8?1:2);
  Tcl_SetObjResult(interp, Tcl_NewByteArrayObj((u8*)z, len));

  sqlite4ValueFree(pVal);

  return TCL_OK;
}

/*
** Usage: translate_selftest
**
** Call sqlite4UtfSelfTest() to run the internal tests for unicode
** translation. If there is a problem an assert() will fail.
**/
void sqlite4UtfSelfTest(void);
static int test_translate_selftest(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifndef SQLITE4_OMIT_UTF16
  sqlite4UtfSelfTest();
#endif
  return SQLITE4_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Sqlitetest5_Init(Tcl_Interp *interp){
  static struct {
    char *zName;
    Tcl_ObjCmdProc *xProc;
  } aCmd[] = {
    { "binarize",                (Tcl_ObjCmdProc*)binarize },
    { "test_value_overhead",     (Tcl_ObjCmdProc*)test_value_overhead },
    { "test_translate",          (Tcl_ObjCmdProc*)test_translate     },
    { "sqlite4_translate",       (Tcl_ObjCmdProc*)test_sqlite4_translate  },
    { "translate_selftest",      (Tcl_ObjCmdProc*)test_translate_selftest},
  };
  int i;
  for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
    Tcl_CreateObjCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
  }
  return SQLITE4_OK;
}
