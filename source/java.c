/*
 * java.c
 *
 * FalsoJNI implementation for PAC-MAN Dash! (minimal first-build version).
 *
 * All dispatch tables are empty: FalsoJNI logs every Java method/field the game
 * asks for that is not registered. Run the game once, read the log, and add the
 * entries here (same pattern as the CE DX java.c: an ID enum, nameToMethodId,
 * and one MethodsXxx[] table per return type).
 *
 * This file also replaces the CE DX one, so none of its APKFileHelper/FMOD
 * code is referenced any more.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "utils/logger.h"

NameToMethodID nameToMethodId[] = {};

MethodsBoolean methodsBoolean[] = {};
MethodsByte    methodsByte[]    = {};
MethodsChar    methodsChar[]    = {};
MethodsDouble  methodsDouble[]  = {};
MethodsFloat   methodsFloat[]   = {};
MethodsLong    methodsLong[]    = {};
MethodsShort   methodsShort[]   = {};
MethodsInt     methodsInt[]     = {};
MethodsObject  methodsObject[]  = {};
MethodsVoid    methodsVoid[]    = {};

NameToFieldID nameToFieldId[] = {};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte    fieldsByte[]    = {};
FieldsChar    fieldsChar[]    = {};
FieldsDouble  fieldsDouble[]  = {};
FieldsFloat   fieldsFloat[]   = {};
FieldsInt     fieldsInt[]     = {};
FieldsObject  fieldsObject[]  = {};
FieldsLong    fieldsLong[]    = {};
FieldsShort   fieldsShort[]   = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
