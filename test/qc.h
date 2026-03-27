/********************************************************************
 * Copyright (c) 2014, Andrea Zito
 * All rights reserved.
 *
 * License: BSD3
 ********************************************************************/

#ifndef QUICKCHECK4C_C
#define QUICKCHECK4C_C

#include <stdarg.h>
#include <stdlib.h>

/* Default ranges used for type generation */
#define QCC_LONG_FROM -RAND_MAX / 2
#define QCC_LONG_TO RAND_MAX / 2
#define QCC_INT_FROM -RAND_MAX / 2
#define QCC_INT_TO RAND_MAX / 2
#define QCC_DOUBLE_FROM ((double)-(RAND_MAX / 2))
#define QCC_DOUBLE_TO ((double)(RAND_MAX / 2))
#define QCC_FLOAT_FROM ((float)-(RAND_MAX / 2))
#define QCC_FLOAT_TO ((float)(RAND_MAX / 2))

/**
 * Explicit definition of a boolean type compatible with
 * normal C boolean interpretation.
 */
typedef enum { QCC_TRUE = 1, QCC_FALSE = 0 } QCC_Boolean;

/**
 * Enumerator defining the evaluation of a property.
 * QCC_OK represent a satisfied property.
 * QCC_FAIL represent a falsified property.
 * QCC_NOTHING indicates a property that didn't fail
 * but should NOT be considered in the successes count or
 * categorization (used by QCC_IMPLY)
 */
typedef enum { QCC_OK = 1, QCC_FAIL = 0, QCC_NOTHING = -1 } QCC_TestStatus;

/**
 * Convenince macro to extract generated values inside a property.
 * Es: int a = *QCC_getValue(vals, 0, int*)
 *
 * @param vals A QCC_GenValue array
 * @param idx  Index of the value to extract
 * @param type Type of the generated value
 */
#define QCC_getValue(vals, idx, type) ((type)vals[idx]->value)

/**
 * Signature of function used to represent values in a human readable
 * fashion.
 * Calling this function allocates memory that must be freed by the caller.
 *
 * @param value Pointer to the value to show
 * @param len Lenght of the data (used to keep track of array lengths)
 * @return String representation of the value.
 */
typedef char *(*QCC_showValue)(void *value, int len);

/**
 * Signature of function used to free generated values.
 *
 * @param value Pointer to the value to free
 */
typedef void (*QCC_freeValue)(void *value);

/**
 * Structure defining a generated value.
 * It specifies the value itself (alongside its length in case
 * of arrays), a function pointer for getting a human readable
 * string representation of the value and a function pointer to
 * free the memory allocated for the value.
 *
 * @param value Pointer to the memory allocated for the value
 * @param n Length of the value (1 for simple types, length for arrays)
 * @param show Function to get string representation of the value
 * @param free Function to free the memory of the value
 */
typedef struct QCC_GenValue {
  void *value;
  int n;
  QCC_showValue show;
  QCC_freeValue free;
} QCC_GenValue;

/**
 * Opaque structure used to categorize test instances.
 * See QCC_property and QCC_label.
 */
typedef struct QCC_Stamp QCC_Stamp;

/**
 * Signature of functions used to generate random values
 */
typedef QCC_GenValue *(*QCC_gen)();

/**
 * Signature of property functions.
 *
 * @param vals Array of generated random values
 * @param len Number of generated values
 * @param stamp Used to categorize test instances via QCC_label
 */
typedef QCC_TestStatus (*QCC_property)(QCC_GenValue **vals, int len, QCC_Stamp **stamps);

/**
 * Implements logic implication.
 * The implication is satisfied if both the precondition and the property are
 * satisfied.
 * In case the precondition is not satisfied the special QCC_NOTHING value is
 * returned, indicating that the current test should not be considered as a
 * success.
 *
 * @param precondition A boolean condition
 * @param property A QCC_property to evaluate iff the precondition is satisfied
 * @return QCC_TestStatus
 */
#define QCC_imply(precondition, property) precondition ? property : QCC_NOTHING

/**
 * Convenience function to express property conjunction.
 * The function returns:
 *  - QCC_OK iff prop1Status == prop2Status == QCC_OK
 *  - QCC_FAIL iff prop1Status == QCC_FAIL || prop2Status == QCC_FAIL
 *  - QCC_NOTHING iff prop1Status != QCC_FAIL && prop2Status != QCC_FAIL &&
 *                    (prop1Status ==  QCC_NOTHING || prop2Status == QCC_NOTHING)
 *
 * @param prop1Status The evaluation status of property 1
 * @param prop2Status The evaluation status of property 2
 * @return The evaluation status of the conjunction of property 1 and property 2
 */
QCC_TestStatus QCC_and(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status);

/**
 * Convenience function to express property disjunction.
 * The function returns:
 *  - QCC_OK iff prop1Status == QCC_OK || prop2Status == QCC_OK
 *  - QCC_FAIL iff prop1Status == QCC_FAIL && prop2Status == QCC_FAIL
 *  - QCC_NOTHING iff prop1Status != QCC_OK && prop2Status != QCC_OK &&
 *                    (prop1Status == QCC_NOTHING || prop2Status == QCC_NOTHING)
 *
 * @param prop1Status The evaluation status of property 1
 * @param prop2Status The evaluation status of property 2
 * @return The evaluation status of the disjunction of property 1 and property 2
 */
QCC_TestStatus QCC_or(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status);

/**
 * Convenience function to express property exclusive disjunction.
 * The function returns:
 *  - QCC_OK iff (prop1Status == QCC_OK && prop2Status == QCC_FAIL) ||
 *               (prop1Status == QCC_FAIL && prop2Status == QCC_OK)
 *  - QCC_FAIL iff prop1Status == prop2Status != QCC_NOTHING
 *  - QCC_NOTHING iff prop1Status == QCC_NOTHING || prop2Status = QCC_NOTHING
 *
 * @param prop1Status The evaluation status of property 1
 * @param prop2Status The evaluation status of property 2
 * @return The evaluation status of the exclusive disjunction of property 1 and
 *         property 2
 */
QCC_TestStatus QCC_xor(QCC_TestStatus prop1Status, QCC_TestStatus prop2Status);

/**
 * Convenience function to express property negation.
 * The function returns:
 *  - QCC_OK iff propStatus == QCC_FAIL
 *  - QCC_FAIL iff propStatus == QCC_OK
 *  - QCC_NOTHING iff propStatus == QCC_NOTHING
 */
QCC_TestStatus QCC_not(QCC_TestStatus propStatus);

/**
 * Initialize the random generator using a specific seed.
 *
 * @param seed The seed to use or 0 to automatically select seed
 */
void QCC_init(int seed);

/**
 * Adds a label to the test stamps.
 *
 * @param stamps Stamps associated to the current test
 * @param label Label to add to the test stamps
 */
void QCC_label(QCC_Stamp **stamps, char *label);

/**
 * Test a property for num times allowing at most maxFail unsuccesful
 * argument generation.
 *
 * If all num test succede, the function outputs a success string and
 * return 0. If argument generation fails maxFail time before a success
 * string is printed reporting the number of success test cases and return -1.
 *
 * In both cases any label label gathered during test evaluation is printed
 * alongside its distribution.
 *
 * If a set of arguments falsifying the property is found, the testing is
 * interrupted immediately and a failure string is printed alongside the
 * set of arguments which caused the failure.
 *
 * @parm num Number of successful test to perform
 * @parm maxFail Maximum number of unsuccessful argument generation
 *               (i.e. unsatisfied implication precondition)
 * @parm prop Property to test
 * @parm genNum Number of generators specified as vararg
 * @param ... genNum QCC_gen function to use as generators
 * @return 0 (all num test passed),
 *         -1 (gave up after maxFail unsuccessful arguments generation),
 *         1 (property falsified)
 */
int QCC_testForAll(int num, int maxFail, QCC_property prop, int genNum, ...);

/*************************************************************
 * Helper function for generator definitions
 *************************************************************/

/**
 * Initialize a QCC_GenValue using the specified parameters
 *
 * @param value Raw generated balue
 * @param n Length of the value in case of an array (1 should be used otherwise)
 * @param show Pointer to a QCC_ShowValue function to use for displaying the raw
 * value
 * @param free Pointer to a QCC_FreeValue function to use for free the raw value
 * memory
 * @return Initialized QCC_GenValue
 */
QCC_GenValue *QCC_initGenValue(void *value, int n, QCC_showValue show, QCC_freeValue free);

/*************************************************************
 * Simple types generators
 *************************************************************/
QCC_GenValue *QCC_genLong();
QCC_GenValue *QCC_genLongR(long from, long to);

QCC_GenValue *QCC_genInt();
QCC_GenValue *QCC_genIntR(int from, int to);

QCC_GenValue *QCC_genDouble();
QCC_GenValue *QCC_genDoubleR(double from, double to);

QCC_GenValue *QCC_genFloat();
QCC_GenValue *QCC_genFloatR(float from, float to);

QCC_GenValue *QCC_genBoolean();
QCC_GenValue *QCC_genChar();

/*************************************************************
 * Array types generators
 *************************************************************/
QCC_GenValue *QCC_genString();
QCC_GenValue *QCC_genStringL(int len);

QCC_GenValue *QCC_genArrayByte();
QCC_GenValue *QCC_genArrayByteL(int len);
QCC_GenValue *QCC_genArrayByteLR(int len, long from, long to);

QCC_GenValue *QCC_genArrayLong();
QCC_GenValue *QCC_genArrayLongL(int len);
QCC_GenValue *QCC_genArrayLongLR(int len, long from, long to);

QCC_GenValue *QCC_genArrayInt();
QCC_GenValue *QCC_genArrayIntL(int len);
QCC_GenValue *QCC_genArrayIntLR(int len, int from, int to);

QCC_GenValue *QCC_genArrayDouble();
QCC_GenValue *QCC_genArrayDoubleL(int len);
QCC_GenValue *QCC_genArrayDoubleLR(int len, double from, double to);

QCC_GenValue *QCC_genArrayFloat();
QCC_GenValue *QCC_genArrayFloatL(int len);
QCC_GenValue *QCC_genArrayFloatLR(int len, float from, float to);

QCC_GenValue *QCC_genArrayBoolean();
QCC_GenValue *QCC_genArrayBooleanL(int len);

QCC_GenValue *QCC_genArrayChar();
QCC_GenValue *QCC_genArrayCharL(int len);

QCC_GenValue *QCC_genArrayString();
QCC_GenValue *QCC_genArrayStringL(int len, int strLen);
#endif
