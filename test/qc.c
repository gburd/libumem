/********************************************************************
 * Copyright (c) 2014, Andrea Zito
 * All rights reserved.
 *
 * License: BSD3
 ********************************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "qc.h"

// TODO: fix these...
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wint-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"

typedef void (*QCC_genRaw)(void *ptr);
typedef void (*QCC_genRawR)(void *ptr, void *from, void *to);

struct QCC_Stamp {
  char *label;
  int n;
  struct QCC_Stamp *next;
};

typedef struct QCC_Result {
  QCC_TestStatus status;
  QCC_Stamp *stamps;
  QCC_GenValue **arguments;
  int argumentsN;
} QCC_Result;

enum QCC_deref_type { NONE, LONG, INT, FLOAT, DOUBLE, BYTE, CHAR };

void
QCC_init(int seed)
{
  if (seed) {
    srandom(seed);
  } else {
    srandom(time(NULL));
  }
}

/***********************************************************************
 *  Generators helper functions
 ***********************************************************************/
static char *
QCC_showSimpleValue(void *value, enum QCC_deref_type dt, int maxsize, const char *format)
{
  char *vc = malloc(sizeof(char) * (maxsize + 1));

  switch (dt) {
  case LONG:
    snprintf(vc, maxsize + 1, format, *(long *)value);
    break;
  case INT:
    snprintf(vc, maxsize + 1, format, *(int *)value);
    break;
  case FLOAT:
    snprintf(vc, maxsize + 1, format, *(float *)value);
    break;
  case DOUBLE:
    snprintf(vc, maxsize + 1, format, *(double *)value);
    break;
  case CHAR:
    snprintf(vc, maxsize + 1, format, *(char *)value);
    break;
  case BYTE:
    snprintf(vc, maxsize + 1, format, *(uint8_t *)value);
    break;
  default:
    snprintf(vc, maxsize + 1, format, *(int *)value);
    break;
  }
  vc[maxsize] = 0;
  return vc;
}

static void
QCC_freeSimpleValue(void *value)
{
  free(value);
}

QCC_GenValue *
QCC_initGenValue(void *value, int n, QCC_showValue show, QCC_freeValue free)
{
  QCC_GenValue *gv = malloc(sizeof(QCC_GenValue));
  *gv = (QCC_GenValue) { .value = value, .n = n, .show = show, .free = free };

  return gv;
}

/***********************************************************************
 *  Generators implementations
 ***********************************************************************/

static char *
QCC_showLong(void *value, int len)
{
  return QCC_showSimpleValue(value, LONG, 21, "%ld");
}

void
QCC_genLongAtR(long *l, long *from, long *to)
{
  long _from = *from;
  long _to = *to;
  unsigned long n = _to - _from;

  if (n > RAND_MAX) {
    _from = _from + n / 2 - RAND_MAX / 2;
    _to = _from + n / 2 + RAND_MAX / 2;
    n = _to - _from;
  }
  *l = (random() % n) + _from;
}

void
QCC_genLongAt(long *l)
{
  long from = QCC_LONG_FROM;
  long to = QCC_LONG_TO;
  QCC_genLongAtR(l, &from, &to);
}

QCC_GenValue *
QCC_genLongR(long from, long to)
{
  long *v = malloc(sizeof(long));
  QCC_genLongAtR(v, &from, &to);

  return QCC_initGenValue(v, 1, QCC_showLong, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genLong()
{
  return QCC_genLongR(QCC_LONG_FROM, QCC_LONG_TO);
}

static char *
QCC_showInt(void *value, int len)
{
  return QCC_showSimpleValue(value, INT, 11, "%d");
}

void
QCC_genIntAtR(int *i, int *from, int *to)
{
  int n = *to - *from;
  *i = (random() % n) + *from;
}

void
QCC_genIntAt(int *i)
{
  int from = QCC_INT_FROM;
  int to = QCC_INT_TO;
  QCC_genIntAtR(i, &from, &to);
}

QCC_GenValue *
QCC_genIntR(int from, int to)
{
  int *v = malloc(sizeof(int));
  QCC_genIntAtR(v, &from, &to);

  return QCC_initGenValue(v, 1, QCC_showInt, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genInt()
{
  return QCC_genIntR(QCC_INT_FROM, QCC_INT_TO);
}

static char *
QCC_showDouble(void *value, int len)
{
  return QCC_showSimpleValue(value, DOUBLE, 50, "%.12e");
}

void
QCC_genDoubleAtR(double *d, double *from, double *to)
{
  double r = (double)random() / (double)RAND_MAX;
  *d = *from + (*to - *from) * r;
}

void
QCC_genDoubleAt(double *d)
{
  double from = QCC_DOUBLE_FROM;
  double to = QCC_DOUBLE_TO;
  QCC_genDoubleAtR(d, &from, &to);
}

QCC_GenValue *
QCC_genDoubleR(double from, double to)
{
  double *v = malloc(sizeof(double));
  QCC_genDoubleAtR(v, &from, &to);
  return QCC_initGenValue(v, 1, QCC_showDouble, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genDouble()
{
  return QCC_genDoubleR(QCC_DOUBLE_FROM, QCC_DOUBLE_TO);
}

static char *
QCC_showFloat(void *value, int len)
{
  return QCC_showSimpleValue(value, FLOAT, 50, "%.12e");
}

void
QCC_genFloatAtR(float *f, float *from, float *to)
{
  float r = (float)random() / (float)RAND_MAX;
  *f = *from + (*to - *from) * r;
}

void
QCC_genFloatAt(float *f)
{
  float from = QCC_FLOAT_FROM;
  float to = QCC_FLOAT_TO;
  QCC_genFloatAtR(f, &from, &to);
}

QCC_GenValue *
QCC_genFloatR(float from, float to)
{
  float *v = malloc(sizeof(float));
  QCC_genFloatAtR(v, &from, &to);

  return QCC_initGenValue(v, 1, QCC_showFloat, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genFloat()
{
  return QCC_genFloatR(QCC_FLOAT_FROM, QCC_FLOAT_TO);
}

static char *
QCC_showBoolean(void *value, int len)
{
  QCC_Boolean *b = (QCC_Boolean *)value;

  if (*b) {
    return strdup("TRUE");
  } else {
    return strdup("FALSE");
  }
}

void
QCC_genBooleanAt(QCC_Boolean *b)
{
  double r = (double)random() / (double)RAND_MAX;
  *b = r > 0.5 ? QCC_TRUE : QCC_FALSE;
}

QCC_GenValue *
QCC_genBoolean()
{
  QCC_Boolean *v = malloc(sizeof(QCC_Boolean));
  QCC_genBooleanAt(v);

  return QCC_initGenValue(v, 1, QCC_showBoolean, QCC_freeSimpleValue);
}

static char *
QCC_showChar(void *value, int len)
{
  return QCC_showSimpleValue(value, CHAR, 3, "'%c'");
}

void
QCC_genCharAt(char *c)
{
  *c = (char)(random() % 93) + 33;
}

QCC_GenValue *
QCC_genChar()
{
  char *v = malloc(sizeof(char));
  QCC_genCharAt(v);

  return QCC_initGenValue(v, 1, QCC_showChar, QCC_freeSimpleValue);
}

/***********************************************************************
 *  Array generators implementations
 ***********************************************************************/

QCC_GenValue *
QCC_genArrayOf(int len, QCC_genRaw elemGen, size_t elemSize, QCC_showValue show, QCC_freeValue free)
{
  int n = random() % len;
  uint8_t *arr = malloc(n * elemSize);

  int p, i;
  for (i = 0, p = 0; i < n; i++, p += elemSize)
    elemGen(arr + p);

  return QCC_initGenValue(arr, n, show, free);
}

QCC_GenValue *
QCC_genArrayOfR(int len, QCC_genRawR elemGen, void *from, void *to, size_t elemSize, QCC_showValue show, QCC_freeValue free)
{
  int n = random() % len;
  uint8_t *arr = malloc(n * elemSize);

  int p, i;
  for (i = 0, p = 0; i < n; i++, p += elemSize)
    elemGen(arr + p, from, to);

  return QCC_initGenValue(arr, n, show, free);
}

static char *
QCC_showString(void *value, int len)
{
  return QCC_showSimpleValue(value, NONE, len, "%s");
}

QCC_GenValue *
QCC_genStringL(int len)
{
  QCC_GenValue *s = QCC_genArrayOf(len, (QCC_genRaw)QCC_genCharAt, sizeof(char), QCC_showString, QCC_freeSimpleValue);
  ((char *)s->value)[s->n - 1] = '\0';
  return s;
}

QCC_GenValue *
QCC_genString()
{
  return QCC_genStringL(50);
}

static char *
QCC_showSimpleArray(void *value, size_t elemSize, QCC_showValue showValue, int len)
{
  char **valStr = malloc(sizeof(char *) * len);
  int valStrLen = 0;
  int i;
  for (i = 0; i < len; i++) {
    valStr[i] = showValue(((uint8_t *)value) + (i * elemSize), 1);
    valStrLen += strlen(valStr[i]);
  }

  int totStrLen = valStrLen + 2 + 2 * len + 1;
  char *str = malloc(sizeof(char) * totStrLen);
  sprintf(str, "[");
  int currLen = 1;
  for (i = 0; i < len; i++) {
    if (i == 0) {
      sprintf(str + currLen, "%s", valStr[i]);
    } else {
      sprintf(str + currLen, ", %s", valStr[i]);
      currLen += 2;
    }
    currLen += strlen(valStr[i]);
    free(valStr[i]);
  }
  sprintf(str + currLen, "]");
  free(valStr);
  return str;
}

static char *
QCC_showByte(void *value, int len)
{
  /* Buffer needs space for: ' + up to 2 hex digits + ' + null = 5 chars */
  return QCC_showSimpleValue(value, BYTE, 4, "'%x'");
}

static char *
QCC_showArrayByte(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(long), QCC_showByte, n);
}

void
QCC_genByteAtR(uint8_t *b, uint8_t *from, uint8_t *to)
{
  uint8_t r = (uint8_t)random() / (uint8_t)RAND_MAX;
  *b = *from + (*to - *from) * r;
}

void
QCC_genByteAt(uint8_t *b)
{
  *b = ((uint8_t)random() / (uint8_t)RAND_MAX) % UINT8_MAX;
}

QCC_GenValue *
QCC_genArrayByteLR(int len, long from, long to)
{
  return QCC_genArrayOfR(len, (QCC_genRawR)QCC_genByteAtR, &from, &to, sizeof(long), QCC_showArrayByte, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayByteL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genByteAt, sizeof(long), QCC_showArrayByte, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayByte()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genByteAt, sizeof(long), QCC_showArrayByte, QCC_freeSimpleValue);
}

static char *
QCC_showArrayLong(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(long), QCC_showLong, n);
}

QCC_GenValue *
QCC_genArrayLongLR(int len, long from, long to)
{
  return QCC_genArrayOfR(len, (QCC_genRawR)QCC_genLongAtR, &from, &to, sizeof(long), QCC_showArrayLong, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayLongL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genLongAt, sizeof(long), QCC_showArrayLong, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayLong()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genLongAt, sizeof(long), QCC_showArrayLong, QCC_freeSimpleValue);
}

static char *
QCC_showArrayInt(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(int), QCC_showInt, n);
}

QCC_GenValue *
QCC_genArrayIntLR(int len, int from, int to)
{
  return QCC_genArrayOfR(len, (QCC_genRawR)QCC_genIntAtR, &from, &to, sizeof(int), QCC_showArrayInt, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayIntL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genIntAt, sizeof(int), QCC_showArrayInt, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayInt()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genIntAt, sizeof(int), QCC_showArrayInt, QCC_freeSimpleValue);
}

static char *
QCC_showArrayDouble(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(double), QCC_showDouble, n);
}

QCC_GenValue *
QCC_genArrayDoubleLR(int len, double from, double to)
{
  return QCC_genArrayOfR(len, (QCC_genRawR)QCC_genDoubleAtR, &from, &to, sizeof(double), QCC_showArrayDouble, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayDoubleL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genDoubleAt, sizeof(double), QCC_showArrayDouble, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayDouble()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genDoubleAt, sizeof(double), QCC_showArrayDouble, QCC_freeSimpleValue);
}

static char *
QCC_showArrayFloat(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(float), QCC_showFloat, n);
}

QCC_GenValue *
QCC_genArrayFloatLR(int len, float from, float to)
{
  return QCC_genArrayOfR(len, (QCC_genRawR)QCC_genFloatAtR, &from, &to, sizeof(float), QCC_showArrayFloat, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayFloatL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genFloatAt, sizeof(float), QCC_showArrayFloat, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayFloat()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genFloatAt, sizeof(float), QCC_showArrayFloat, QCC_freeSimpleValue);
}

static char *
QCC_showArrayBoolean(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(QCC_Boolean), QCC_showBoolean, n);
}

QCC_GenValue *
QCC_genArrayBooleanL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genBooleanAt, sizeof(QCC_Boolean), QCC_showArrayBoolean, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayBoolean()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genBooleanAt, sizeof(QCC_Boolean), QCC_showArrayBoolean, QCC_freeSimpleValue);
}

static char *
QCC_showArrayChar(void *value, int n)
{
  return QCC_showSimpleArray(value, sizeof(char), QCC_showChar, n);
}

QCC_GenValue *
QCC_genArrayCharL(int len)
{
  return QCC_genArrayOf(len, (QCC_genRaw)QCC_genCharAt, sizeof(char), QCC_showArrayChar, QCC_freeSimpleValue);
}

QCC_GenValue *
QCC_genArrayChar()
{
  return QCC_genArrayOf(50, (QCC_genRaw)QCC_genCharAt, sizeof(char), QCC_showArrayChar, QCC_freeSimpleValue);
}

/***********************************************************************
 *  Convenience functions
 ***********************************************************************/
QCC_TestStatus
QCC_not(QCC_TestStatus propStatus)
{
  switch (propStatus) {
  case QCC_OK:
    return QCC_FAIL;
  case QCC_FAIL:
    return QCC_OK;
  case QCC_NOTHING:
    return QCC_NOTHING;
  default:
    return QCC_FAIL;
  }
}

QCC_TestStatus
QCC_and(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status)
{
  switch (prop1Status) {
  case QCC_OK:
    return prop2Status;
  case QCC_FAIL:
    return QCC_FAIL;
  case QCC_NOTHING:
    return QCC_NOTHING;
  default:
    return QCC_FAIL;
  }
}

QCC_TestStatus
QCC_or(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status)
{
  switch (prop1Status) {
  case QCC_OK:
    return QCC_OK;
  case QCC_FAIL:
  case QCC_NOTHING:
    return prop2Status;
  default:
    return QCC_FAIL;
  }
}

QCC_TestStatus
QCC_xor(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status)
{
  switch (prop1Status) {
  case QCC_OK:
    return QCC_not(prop2Status);
  case QCC_FAIL:
    return prop2Status;
  case QCC_NOTHING:
    return QCC_NOTHING;
  default:
    return QCC_FAIL;
  }
}

/***********************************************************************
 *  Categorization function
 ***********************************************************************/
void
QCC_freeStamp(QCC_Stamp *stamps)
{
  QCC_Stamp *tstamps;
  QCC_Stamp *curr;
  for (tstamps = stamps; tstamps != NULL;) {
    curr = tstamps;
    tstamps = tstamps->next;
    free(curr->label);
    free(curr);
  }
}

static void
QCC_insertOrdStamp(QCC_Stamp **stamps, QCC_Stamp *s)
{
  QCC_Stamp *new = malloc(sizeof(QCC_Stamp));
  *new = (QCC_Stamp) { .label = strdup(s->label), .n = s->n, .next = NULL };

  QCC_Stamp *tstamp = *stamps;
  QCC_Stamp **pre = stamps;
  while (tstamp) {
    if (tstamp->n < new->n) {
      break;
    } else {
      pre = &tstamp->next;
      tstamp = tstamp->next;
    }
  }
  *pre = new;
  new->next = tstamp;
}

static QCC_Stamp *
QCC_sortStamp(QCC_Stamp *stamps)
{
  QCC_Stamp *sortedStamps = NULL;
  QCC_Stamp *tstamp;
  for (tstamp = stamps; tstamp != NULL; tstamp = tstamp->next) {
    QCC_insertOrdStamp(&sortedStamps, tstamp);
  }
  return sortedStamps;
}

static void
QCC_labelN(QCC_Stamp **stamps, char *label, int n)
{
  QCC_Stamp *ptr = *stamps;
  QCC_Stamp *pre = NULL;
  QCC_Stamp *new;

  while (ptr) {
    if (strcmp(ptr->label, label) == 0) {
      (ptr->n)++;
      return;
    }
    pre = ptr;
    ptr = ptr->next;
  }

  new = malloc(sizeof(QCC_Stamp));
  *new = (QCC_Stamp) { .label = strdup(label), .n = 1, .next = NULL };

  if (pre) {
    pre->next = new;
  } else {
    *stamps = new;
  }
}

void
QCC_label(QCC_Stamp **stamps, char *label)
{
  return QCC_labelN(stamps, label, 1);
}

static void
QCC_mergeLabels(QCC_Stamp **dst, QCC_Stamp *src)
{
  for (; src != NULL; src = src->next)
    QCC_labelN(dst, src->label, src->n);
}

/***********************************************************************
 *  Testing functions
 ***********************************************************************/
void
QCC_freeGenValues(QCC_GenValue **arguments, int argumentsN)
{
  int i;
  for (i = 0; i < argumentsN; i++) {
    arguments[i]->free(arguments[i]->value);
    free(arguments[i]);
  }
  if (arguments)
    free(arguments);
}

void
QCC_freeResult(QCC_Result *res)
{
  QCC_freeStamp(res->stamps);
  QCC_freeGenValues(res->arguments, res->argumentsN);
}

QCC_Result
QCC_vforAll(QCC_property prop, int genNum, va_list genP)
{ // QCC_gen *genLst,
  QCC_GenValue **vals = NULL;
  if (genNum) {
    int i;
    vals = malloc(sizeof(QCC_GenValue *) * genNum);
    for (i = 0; i < genNum; i++) {
      QCC_gen gen = va_arg(genP, QCC_gen);
      vals[i] = gen();
    }
  }

  QCC_Stamp *stamps = NULL;
  QCC_TestStatus status = prop(vals, genNum, &stamps);
  return (QCC_Result) { .status = status, .stamps = stamps, .arguments = vals, .argumentsN = genNum };
}

QCC_Result
QCC_forAll(QCC_property prop, int genNum, ...)
{ // QCC_gen *genLst,
  va_list genP;
  QCC_Result res;
  va_start(genP, genNum);
  res = QCC_vforAll(prop, genNum, genP);
  va_end(genP);
  return res;
}

static void
QCC_printStamps(QCC_Stamp *stamps, int n)
{
  QCC_Stamp *sortedStamps = QCC_sortStamp(stamps);
  QCC_Stamp *tstamps;
  for (tstamps = sortedStamps; tstamps != NULL; tstamps = tstamps->next)
    printf("%.2f%%\t%s\n", (tstamps->n / (float)n) * 100, tstamps->label);
  if (sortedStamps)
    QCC_freeStamp(sortedStamps);
}

static void
QCC_printArguments(QCC_GenValue **arguments, int argumentsN)
{
  int i;
  for (i = 0; i < argumentsN; i++) {
    char *s = arguments[i]->show(arguments[i]->value, arguments[i]->n);
    printf("%s\n", s);
    free(s);
  }
}

int
QCC_testForAll(int num, int maxFail, QCC_property prop, int genNum, ...)
{
  va_list genP;
  int succ = 0;
  int fail = 0;

  QCC_Result res = { .status = QCC_OK };
  QCC_Stamp *stamps = NULL;
  while (succ < num && fail < maxFail) {
    va_start(genP, genNum);
    res = QCC_vforAll(prop, genNum, genP);
    va_end(genP);

    if (res.status == QCC_FAIL) {
      break;
    } else {
      if (res.status == QCC_OK) {
        succ++;
        QCC_mergeLabels(&stamps, res.stamps);
      } else
        fail++;
      QCC_freeResult(&res);
    }
  }

  if (succ == num) {
    //printf("%d test passed (%d)!\n", succ, fail);
    QCC_printStamps(stamps, succ);
    if (stamps)
      QCC_freeStamp(stamps);
    return 0;
  } else if (res.status == QCC_FAIL) {
    printf("\nFalsifiable after %d test\n", succ + 1);
    QCC_printArguments(res.arguments, res.argumentsN);
    QCC_freeResult(&res);
    if (stamps)
      QCC_freeStamp(stamps);
    return 1;
  } else if (fail >= maxFail) {
    printf("\nGave up after %d tests!\n", succ);
    QCC_printStamps(stamps, succ);
    if (stamps)
      QCC_freeStamp(stamps);
    return -1;
  }

  return 0;
}
#pragma GCC diagnostic pop
