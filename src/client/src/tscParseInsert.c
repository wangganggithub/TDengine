/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE /* See feature_test_macros(7) */
#define _GNU_SOURCE

#define _XOPEN_SOURCE

#pragma GCC diagnostic ignored "-Woverflow"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "os.h"
#include "ihash.h"
#include "tscSecondaryMerge.h"
#include "tscUtil.h"
#include "tschemautil.h"
#include "tsclient.h"
#include "tsqldef.h"
#include "ttypes.h"

#include "tlog.h"
#include "tstoken.h"
#include "ttime.h"

enum {
  TSDB_USE_SERVER_TS = 0,
  TSDB_USE_CLI_TS = 1,
};

static int32_t tscAllocateMemIfNeed(STableDataBlocks *pDataBlock, int32_t rowSize);

static int32_t tscToInteger(SSQLToken *pToken, int64_t *value, char **endPtr) {
  int32_t numType = isValidNumber(pToken);
  if (TK_ILLEGAL == numType) {
    return numType;
  }

  int32_t radix = 10;
  if (numType == TK_HEX) {
    radix = 16;
  } else if (numType == TK_OCT) {
    radix = 8;
  } else if (numType == TK_BIN) {
    radix = 2;
  }

  errno = 0;
  *value = strtoll(pToken->z, endPtr, radix);

  return numType;
}

static int32_t tscToDouble(SSQLToken *pToken, double *value, char **endPtr) {
  int32_t numType = isValidNumber(pToken);
  if (TK_ILLEGAL == numType) {
    return numType;
  }

  errno = 0;
  *value = strtod(pToken->z, endPtr);
  return numType;
}

int tsParseTime(SSQLToken *pToken, int64_t *time, char **next, char *error, int16_t timePrec) {
  //char *    token; //fang not used
  //int       tokenlen; //fang not used
  int32_t   index = 0;
  SSQLToken sToken;
  int64_t   interval;
  int64_t   useconds = 0;
  char *    pTokenEnd = *next;

  index = 0;

  if (pToken->type == TK_NOW) {
    useconds = taosGetTimestamp(timePrec);
  } else if (strncmp(pToken->z, "0", 1) == 0 && pToken->n == 1) {
    // do nothing
  } else if (pToken->type == TK_INTEGER) {
    useconds = str2int64(pToken->z);
  } else {
    // strptime("2001-11-12 18:31:01", "%Y-%m-%d %H:%M:%S", &tm);
    if (taosParseTime(pToken->z, time, pToken->n, timePrec) != TSDB_CODE_SUCCESS) {
      return tscInvalidSQLErrMsg(error, "invalid timestamp format", pToken->z);
    }

    return TSDB_CODE_SUCCESS;
  }

  for (int k = pToken->n; pToken->z[k] != '\0'; k++) {
    if (pToken->z[k] == ' ' || pToken->z[k] == '\t') continue;
    if (pToken->z[k] == ',') {
      *next = pTokenEnd;
      *time = useconds;
      return 0;
    }

    break;
  }

  /*
   * time expression:
   * e.g., now+12a, now-5h
   */
  SSQLToken valueToken;
  index = 0;
  sToken = tStrGetToken(pTokenEnd, &index, false, 0, NULL);
  pTokenEnd += index;
  
  if (sToken.type == TK_MINUS || sToken.type == TK_PLUS) {
    
    index = 0;
    valueToken = tStrGetToken(pTokenEnd, &index, false, 0, NULL);
    pTokenEnd += index;
    
    if (valueToken.n < 2) {
      return tscInvalidSQLErrMsg(error, "value expected in timestamp", sToken.z);
    }

    if (getTimestampInUsFromStr(valueToken.z, valueToken.n, &interval) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_INVALID_SQL;
    }
    
    if (timePrec == TSDB_TIME_PRECISION_MILLI) {
      interval /= 1000;
    }

    if (sToken.type == TK_PLUS) {
      useconds += interval;
    } else {
      useconds = (useconds >= interval) ? useconds - interval : 0;
    }

    *next = pTokenEnd;
  }

  *time = useconds;
  return TSDB_CODE_SUCCESS;
}

int32_t tsParseOneColumnData(SSchema *pSchema, SSQLToken *pToken, char *payload, char *msg, char **str, bool primaryKey,
                             int16_t timePrec) {
  int64_t iv;
  int32_t numType;
  char *  endptr = NULL;
  errno = 0;   // clear the previous existed error information
  
  switch (pSchema->type) {
    case TSDB_DATA_TYPE_BOOL: {  // bool
      if ((pToken->type == TK_BOOL || pToken->type == TK_STRING) && (pToken->n != 0)) {
        if (strncmp(pToken->z, "true", pToken->n) == 0) {
          *(uint8_t *)payload = TSDB_TRUE;
        } else if (strncmp(pToken->z, "false", pToken->n) == 0) {
          *(uint8_t *)payload = TSDB_FALSE;
        } else if (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0) {
          *(uint8_t *)payload = TSDB_DATA_BOOL_NULL;
        } else {
          return tscInvalidSQLErrMsg(msg, "invalid bool data", pToken->z);
        }
      } else if (pToken->type == TK_INTEGER) {
        iv = strtoll(pToken->z, NULL, 10);
        *(uint8_t *)payload = (int8_t)((iv == 0) ? TSDB_FALSE : TSDB_TRUE);
      } else if (pToken->type == TK_FLOAT) {
        double dv = strtod(pToken->z, NULL);
        *(uint8_t *)payload = (int8_t)((dv == 0) ? TSDB_FALSE : TSDB_TRUE);
      } else if (pToken->type == TK_NULL) {
        *(uint8_t *)payload = TSDB_DATA_BOOL_NULL;
      } else {
        return tscInvalidSQLErrMsg(msg, "invalid bool data", pToken->z);
      }
      break;
    }
    case TSDB_DATA_TYPE_TINYINT:
      if (pToken->type == TK_NULL) {
        *((int8_t *)payload) = TSDB_DATA_TINYINT_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int8_t *)payload) = TSDB_DATA_TINYINT_NULL;
      } else {
        numType = tscToInteger(pToken, &iv, &endptr);
        if (TK_ILLEGAL == numType) {
          return tscInvalidSQLErrMsg(msg, "invalid tinyint data", pToken->z);
        } else if (errno == ERANGE || iv > INT8_MAX || iv <= INT8_MIN) {
          return tscInvalidSQLErrMsg(msg, "tinyint data overflow", pToken->z);
        }

        *((int8_t *)payload) = (int8_t) iv;
      }

      break;

    case TSDB_DATA_TYPE_SMALLINT:
      if (pToken->type == TK_NULL) {
        *((int16_t *)payload) = TSDB_DATA_SMALLINT_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int16_t *)payload) = TSDB_DATA_SMALLINT_NULL;
      } else {
        numType = tscToInteger(pToken, &iv, &endptr);
        if (TK_ILLEGAL == numType) {
          return tscInvalidSQLErrMsg(msg, "invalid smallint data", pToken->z);
        } else if (errno == ERANGE || iv > INT16_MAX || iv <= INT16_MIN) {
          return tscInvalidSQLErrMsg(msg, "smallint data overflow", pToken->z);
        }

        *((int16_t *)payload) = (int16_t)iv;
      }
      break;

    case TSDB_DATA_TYPE_INT:
      if (pToken->type == TK_NULL) {
        *((int32_t *)payload) = TSDB_DATA_INT_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int32_t *)payload) = TSDB_DATA_INT_NULL;
      } else {
        numType = tscToInteger(pToken, &iv, &endptr);
        if (TK_ILLEGAL == numType) {
          return tscInvalidSQLErrMsg(msg, "invalid int data", pToken->z);
        } else if (errno == ERANGE || iv > INT32_MAX || iv <= INT32_MIN) {
          return tscInvalidSQLErrMsg(msg, "int data overflow", pToken->z);
        }

        *((int32_t *)payload) = (int32_t)iv;
      }

      break;

    case TSDB_DATA_TYPE_BIGINT:
      if (pToken->type == TK_NULL) {
        *((int64_t *)payload) = TSDB_DATA_BIGINT_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int64_t *)payload) = TSDB_DATA_BIGINT_NULL;
      } else {
        numType = tscToInteger(pToken, &iv, &endptr);
        if (TK_ILLEGAL == numType) {
          return tscInvalidSQLErrMsg(msg, "invalid bigint data", pToken->z);
        } else if (errno == ERANGE || iv > INT64_MAX || iv <= INT64_MIN) {
          return tscInvalidSQLErrMsg(msg, "bigint data overflow", pToken->z);
        }

        *((int64_t *)payload) = iv;
      }
      break;

    case TSDB_DATA_TYPE_FLOAT:
      if (pToken->type == TK_NULL) {
        *((int32_t *)payload) = TSDB_DATA_FLOAT_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int32_t *)payload) = TSDB_DATA_FLOAT_NULL;
      } else {
        double dv;
        if (TK_ILLEGAL == tscToDouble(pToken, &dv, &endptr)) {
          return tscInvalidSQLErrMsg(msg, "illegal float data", pToken->z);
        }

        float fv = (float)dv;
        if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || (fv > FLT_MAX || fv < -FLT_MAX)) {
          return tscInvalidSQLErrMsg(msg, "illegal float data", pToken->z);
        }

        if (isinf(fv) || isnan(fv)) {
          *((int32_t *)payload) = TSDB_DATA_FLOAT_NULL;
        }

        *((float *)payload) = fv;
      }
      break;

    case TSDB_DATA_TYPE_DOUBLE:
      if (pToken->type == TK_NULL) {
        *((int64_t *)payload) = TSDB_DATA_DOUBLE_NULL;
      } else if ((pToken->type == TK_STRING) && (pToken->n != 0) &&
                 (strncasecmp(TSDB_DATA_NULL_STR_L, pToken->z, pToken->n) == 0)) {
        *((int64_t *)payload) = TSDB_DATA_DOUBLE_NULL;
      } else {
        double dv;
        if (TK_ILLEGAL == tscToDouble(pToken, &dv, &endptr)) {
          return tscInvalidSQLErrMsg(msg, "illegal double data", pToken->z);
        }

        if (((dv == HUGE_VAL || dv == -HUGE_VAL) && errno == ERANGE) || (dv > DBL_MAX || dv < -DBL_MAX)) {
          return tscInvalidSQLErrMsg(msg, "illegal double data", pToken->z);
        }

        if (isinf(dv) || isnan(dv)) {
          *((int64_t *)payload) = TSDB_DATA_DOUBLE_NULL;
        } else {
          *((double *)payload) = dv;
        }
      }
      break;

    case TSDB_DATA_TYPE_BINARY:
      // binary data cannot be null-terminated char string, otherwise the last char of the string is lost
      if (pToken->type == TK_NULL) {
        *payload = TSDB_DATA_BINARY_NULL;
      } else { // too long values will return invalid sql, not be truncated automatically
        if (pToken->n > pSchema->bytes) {
          return tscInvalidSQLErrMsg(msg, "string data overflow", pToken->z);
        }
        
        strncpy(payload, pToken->z, pToken->n);
      }

      break;

    case TSDB_DATA_TYPE_NCHAR:
      if (pToken->type == TK_NULL) {
        *(uint32_t *)payload = TSDB_DATA_NCHAR_NULL;
      } else {
        // if the converted output len is over than pSchema->bytes, return error: 'Argument list too long'
        if (!taosMbsToUcs4(pToken->z, pToken->n, payload, pSchema->bytes)) {
          char buf[512] = {0};
          snprintf(buf, 512, "%s", strerror(errno));
          
          return tscInvalidSQLErrMsg(msg, buf, pToken->z);
        }
      }
      break;

    case TSDB_DATA_TYPE_TIMESTAMP: {
      if (pToken->type == TK_NULL) {
        if (primaryKey) {
          *((int64_t *)payload) = 0;
        } else {
          *((int64_t *)payload) = TSDB_DATA_BIGINT_NULL;
        }
      } else {
        int64_t temp;
        if (tsParseTime(pToken, &temp, str, msg, timePrec) != TSDB_CODE_SUCCESS) {
          return tscInvalidSQLErrMsg(msg, "invalid timestamp", pToken->z);
        }
        
        *((int64_t *)payload) = temp;
      }

      break;
    }
  }

  return TSDB_CODE_SUCCESS;
}

/*
 * The server time/client time should not be mixed up in one sql string
 * Do not employ sort operation is not involved if server time is used.
 */
static int32_t tsCheckTimestamp(STableDataBlocks *pDataBlocks, const char *start) {
  // once the data block is disordered, we do NOT keep previous timestamp any more
  if (!pDataBlocks->ordered) {
    return TSDB_CODE_SUCCESS;
  }

  TSKEY k = *(TSKEY *)start;

  if (k == 0) {
    if (pDataBlocks->tsSource == TSDB_USE_CLI_TS) {
      return -1;
    } else if (pDataBlocks->tsSource == -1) {
      pDataBlocks->tsSource = TSDB_USE_SERVER_TS;
    }
  } else {
    if (pDataBlocks->tsSource == TSDB_USE_SERVER_TS) {
      return -1;  // client time/server time can not be mixed
      
    } else if (pDataBlocks->tsSource == -1) {
      pDataBlocks->tsSource = TSDB_USE_CLI_TS;
    }
  }

  if (k <= pDataBlocks->prevTS && (pDataBlocks->tsSource == TSDB_USE_CLI_TS)) {
    pDataBlocks->ordered = false;
  }

  pDataBlocks->prevTS = k;
  return TSDB_CODE_SUCCESS;
}

int tsParseOneRowData(char **str, STableDataBlocks *pDataBlocks, SSchema schema[], SParsedDataColInfo *spd, char *error,
                      int16_t timePrec, int32_t *code, char* tmpTokenBuf) {
  int32_t   index = 0;
  //bool      isPrevOptr; //fang, never used
  SSQLToken sToken = {0};
  char *    payload = pDataBlocks->pData + pDataBlocks->size;

  // 1. set the parsed value from sql string
  int32_t rowSize = 0;
  for (int i = 0; i < spd->numOfAssignedCols; ++i) {
    // the start position in data block buffer of current value in sql
    char *  start = payload + spd->elems[i].offset;
    int16_t colIndex = spd->elems[i].colIndex;
    SSchema *pSchema = schema + colIndex;
    rowSize += pSchema->bytes;

    index = 0;
    sToken = tStrGetToken(*str, &index, true, 0, NULL);
    *str += index;

    if (sToken.type == TK_QUESTION) {
      uint32_t offset = start - pDataBlocks->pData;
      if (tscAddParamToDataBlock(pDataBlocks, pSchema->type, (uint8_t)timePrec, pSchema->bytes, offset) != NULL) {
        continue;
      }
      
      strcpy(error, "client out of memory");
      *code = TSDB_CODE_CLI_OUT_OF_MEMORY;
      return -1;
    }

    if (((sToken.type != TK_NOW) && (sToken.type != TK_INTEGER) && (sToken.type != TK_STRING) &&
         (sToken.type != TK_FLOAT) && (sToken.type != TK_BOOL) && (sToken.type != TK_NULL)) ||
        (sToken.n == 0) || (sToken.type == TK_RP)) {
      tscInvalidSQLErrMsg(error, "invalid data or symbol", sToken.z);
      *code = TSDB_CODE_INVALID_SQL;
      return -1;
    }

    // Remove quotation marks
    if (TK_STRING == sToken.type) {
      // delete escape character: \\, \', \"
      char delim = sToken.z[0];
      int32_t cnt = 0;
      int32_t j = 0;
      for (int32_t k = 1; k < sToken.n - 1; ++k) {  
        if (sToken.z[k] == delim || sToken.z[k] == '\\') {
          if (sToken.z[k + 1] == delim) {
            cnt++;
            continue;
          }
        }
      
        tmpTokenBuf[j] = sToken.z[i];
        j++;
      }
      tmpTokenBuf[j] = 0; 
      sToken.z = tmpTokenBuf;
      sToken.n -= 2 + cnt;    
    }

    bool    isPrimaryKey = (colIndex == PRIMARYKEY_TIMESTAMP_COL_INDEX);
    int32_t ret = tsParseOneColumnData(pSchema, &sToken, start, error, str, isPrimaryKey, timePrec);
    if (ret != TSDB_CODE_SUCCESS) {
      *code = TSDB_CODE_INVALID_SQL;
      return -1;  // NOTE: here 0 mean error!
    }

    if (isPrimaryKey && tsCheckTimestamp(pDataBlocks, start) != TSDB_CODE_SUCCESS) {
      tscInvalidSQLErrMsg(error, "client time/server time can not be mixed up", sToken.z);
      *code = TSDB_CODE_INVALID_TIME_STAMP;
      return -1;
    }
  }

  // 2. set the null value for the columns that do not assign values
  if (spd->numOfAssignedCols < spd->numOfCols) {
    char *ptr = payload;

    for (int32_t i = 0; i < spd->numOfCols; ++i) {
      if (!spd->hasVal[i]) { // current column do not have any value to insert, set it to null
        setNull(ptr, schema[i].type, schema[i].bytes);
      }

      ptr += schema[i].bytes;
    }

    rowSize = ptr - payload;
  }

  return rowSize;
}

static int32_t rowDataCompar(const void *lhs, const void *rhs) {
  TSKEY left = *(TSKEY *)lhs;
  TSKEY right = *(TSKEY *)rhs;

  if (left == right) {
    return 0;
  } else {
    return left > right ? 1 : -1;
  }
}

int tsParseValues(char **str, STableDataBlocks *pDataBlock, SMeterMeta *pMeterMeta, int maxRows,
                  SParsedDataColInfo *spd, char *error, int32_t *code, char* tmpTokenBuf) {
  int32_t   index = 0;
  SSQLToken sToken;

  int16_t numOfRows = 0;

  SSchema *pSchema = tsGetSchema(pMeterMeta);
  int32_t  precision = pMeterMeta->precision;

  if (spd->hasVal[0] == false) {
    strcpy(error, "primary timestamp column can not be null");
    *code = TSDB_CODE_INVALID_SQL;
    return -1;
  }

  while (1) {
    index = 0;
    sToken = tStrGetToken(*str, &index, false, 0, NULL);
    if (sToken.n == 0 || sToken.type != TK_LP) break;

    *str += index;
    if (numOfRows >= maxRows || pDataBlock->size + pMeterMeta->rowSize >= pDataBlock->nAllocSize) {
      int32_t tSize = tscAllocateMemIfNeed(pDataBlock, pMeterMeta->rowSize);
      if (0 == tSize) {  //TODO pass the correct error code to client
        strcpy(error, "client out of memory");
        *code = TSDB_CODE_CLI_OUT_OF_MEMORY;
        return -1;
      }
      
      maxRows += tSize;
    }

    int32_t len = tsParseOneRowData(str, pDataBlock, pSchema, spd, error, precision, code, tmpTokenBuf);
    if (len <= 0) { // error message has been set in tsParseOneRowData
      return -1;
    }

    pDataBlock->size += len;

    index = 0;
    sToken = tStrGetToken(*str, &index, false, 0, NULL);
    *str += index;
    if (sToken.n == 0 || sToken.type != TK_RP) {
      tscInvalidSQLErrMsg(error, ") expected", *str);
      *code = TSDB_CODE_INVALID_SQL;
      return -1;
    }

    numOfRows++;
  }

  if (numOfRows <= 0) {
    strcpy(error, "no any data points");
    *code = TSDB_CODE_INVALID_SQL;
    return -1;
  } else {
    return numOfRows;
  }
}

static void tscSetAssignedColumnInfo(SParsedDataColInfo *spd, SSchema *pSchema, int32_t numOfCols) {
  spd->numOfCols = numOfCols;
  spd->numOfAssignedCols = numOfCols;

  for (int32_t i = 0; i < numOfCols; ++i) {
    spd->hasVal[i] = true;
    spd->elems[i].colIndex = i;

    if (i > 0) {
      spd->elems[i].offset = spd->elems[i - 1].offset + pSchema[i - 1].bytes;
    }
  }
}

int32_t tscAllocateMemIfNeed(STableDataBlocks *pDataBlock, int32_t rowSize) {
  size_t    remain = pDataBlock->nAllocSize - pDataBlock->size;
  const int factor = 5;
  uint32_t nAllocSizeOld = pDataBlock->nAllocSize;

  // expand the allocated size
  if (remain < rowSize * factor) {
    while (remain < rowSize * factor) {
      pDataBlock->nAllocSize = (uint32_t)(pDataBlock->nAllocSize * 1.5);
      remain = pDataBlock->nAllocSize - pDataBlock->size;
    }

    char *tmp = realloc(pDataBlock->pData, (size_t)pDataBlock->nAllocSize);
    if (tmp != NULL) {
      pDataBlock->pData = tmp;
      memset(pDataBlock->pData + pDataBlock->size, 0, pDataBlock->nAllocSize - pDataBlock->size);
    } else {
      //assert(false);
      // do nothing
      pDataBlock->nAllocSize = nAllocSizeOld;
      return 0;
    }
  }

  return (int32_t)(pDataBlock->nAllocSize - pDataBlock->size) / rowSize;
}

static void tsSetBlockInfo(SShellSubmitBlock *pBlocks, const SMeterMeta *pMeterMeta, int32_t numOfRows) {
  pBlocks->sid = pMeterMeta->sid;
  pBlocks->uid = pMeterMeta->uid;
  pBlocks->sversion = pMeterMeta->sversion;
  pBlocks->numOfRows += numOfRows;
}

// data block is disordered, sort it in ascending order
void sortRemoveDuplicates(STableDataBlocks *dataBuf) {
  SShellSubmitBlock *pBlocks = (SShellSubmitBlock *)dataBuf->pData;

  // size is less than the total size, since duplicated rows may be removed yet.
  assert(pBlocks->numOfRows * dataBuf->rowSize + sizeof(SShellSubmitBlock) == dataBuf->size);

  // if use server time, this block must be ordered
  if (dataBuf->tsSource == TSDB_USE_SERVER_TS) {
    assert(dataBuf->ordered);
  }

  if (!dataBuf->ordered) {
    char *pBlockData = pBlocks->payLoad;
    qsort(pBlockData, pBlocks->numOfRows, dataBuf->rowSize, rowDataCompar);

    int32_t i = 0;
    int32_t j = 1;

    while (j < pBlocks->numOfRows) {
      TSKEY ti = *(TSKEY *)(pBlockData + dataBuf->rowSize * i);
      TSKEY tj = *(TSKEY *)(pBlockData + dataBuf->rowSize * j);

      if (ti == tj) {
        ++j;
        continue;
      }

      int32_t nextPos = (++i);
      if (nextPos != j) {
        memmove(pBlockData + dataBuf->rowSize * nextPos, pBlockData + dataBuf->rowSize * j, dataBuf->rowSize);
      }

      ++j;
    }

    dataBuf->ordered = true;

    pBlocks->numOfRows = i + 1;
    dataBuf->size = sizeof(SShellSubmitBlock) + dataBuf->rowSize * pBlocks->numOfRows;
  }
}

static int32_t doParseInsertStatement(SSqlObj *pSql, void *pTableHashList, char **str, SParsedDataColInfo *spd,
                                      int32_t *totalNum) {
  SSqlCmd *       pCmd = &pSql->cmd;
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0);
  SMeterMeta *    pMeterMeta = pMeterMetaInfo->pMeterMeta;

  STableDataBlocks *dataBuf =
      tscGetDataBlockFromList(pTableHashList, pCmd->pDataBlocks, pMeterMeta->uid, TSDB_DEFAULT_PAYLOAD_SIZE,
                              sizeof(SShellSubmitBlock), pMeterMeta->rowSize, pMeterMetaInfo->name);

  int32_t maxNumOfRows = tscAllocateMemIfNeed(dataBuf, pMeterMeta->rowSize);
  if (0 == maxNumOfRows) {
    return TSDB_CODE_CLI_OUT_OF_MEMORY;
  }
  
  int32_t code = TSDB_CODE_INVALID_SQL;
  char*   tmpTokenBuf = calloc(1, 4096);  // used for deleting Escape character: \\, \', \"
  if (NULL == tmpTokenBuf) {
    return TSDB_CODE_CLI_OUT_OF_MEMORY;
  }
  
  int32_t numOfRows = tsParseValues(str, dataBuf, pMeterMeta, maxNumOfRows, spd, pCmd->payload, &code, tmpTokenBuf);
  free(tmpTokenBuf);
  if (numOfRows <= 0) {
    return code;
  }

  for (uint32_t i = 0; i < dataBuf->numOfParams; ++i) {
    SParamInfo* param = dataBuf->params + i;
    if (param->idx == -1) {
      param->idx = pCmd->numOfParams++;
      param->offset -= sizeof(SShellSubmitBlock);
    }
  }

  SShellSubmitBlock *pBlocks = (SShellSubmitBlock *)(dataBuf->pData);
  tsSetBlockInfo(pBlocks, pMeterMeta, numOfRows);

  dataBuf->vgid = pMeterMeta->vgid;
  dataBuf->numOfMeters = 1;

  /*
   * the value of pRes->numOfRows does not affect the true result of AFFECTED ROWS,
   * which is actually returned from server.
   */
  *totalNum += numOfRows;
  return TSDB_CODE_SUCCESS;
}

static int32_t tscParseSqlForCreateTableOnDemand(char **sqlstr, SSqlObj *pSql) {
  int32_t   index = 0;
  SSQLToken sToken;
  SSQLToken tableToken;
  int32_t   code = TSDB_CODE_SUCCESS;

  SSqlCmd *       pCmd = &pSql->cmd;
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0);

  char *sql = *sqlstr;
  // get the token of specified table
  index = 0;
  tableToken = tStrGetToken(sql, &index, false, 0, NULL);
  sql += index;

  char *cstart = NULL;
  char *cend = NULL;

  // skip possibly exists column list
  index = 0;
  sToken = tStrGetToken(sql, &index, false, 0, NULL);
  sql += index;

  int32_t numOfColList = 0;
  bool    createTable = false;

  if (sToken.type == TK_LP) {
    cstart = &sToken.z[0];
    index = 0;
    while (1) {
      sToken = tStrGetToken(sql, &index, false, 0, NULL);
      if (sToken.type == TK_RP) {
        cend = &sToken.z[0];
        break;
      }

      ++numOfColList;
    }

    sToken = tStrGetToken(sql, &index, false, 0, NULL);
    sql += index;
  }

  if (numOfColList == 0 && cstart != NULL) {
    return TSDB_CODE_INVALID_SQL;
  }

  if (sToken.type == TK_USING) { // create table if not exists
    index = 0;
    sToken = tStrGetToken(sql, &index, false, 0, NULL);
    sql += index;

    STagData *pTag = (STagData *)pCmd->payload;
    memset(pTag, 0, sizeof(STagData));
    setMeterID(pSql, &sToken, 0);

    strncpy(pTag->name, pMeterMetaInfo->name, TSDB_METER_ID_LEN);
    code = tscGetMeterMeta(pSql, pTag->name, 0);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    if (!UTIL_METER_IS_METRIC(pMeterMetaInfo)) {
      return tscInvalidSQLErrMsg(pCmd->payload, "create table only from super table is allowed", sToken.z);
    }

    char *   tagVal = pTag->data;
    SSchema *pTagSchema = tsGetTagSchema(pMeterMetaInfo->pMeterMeta);

    index = 0;
    sToken = tStrGetToken(sql, &index, false, 0, NULL);
    sql += index;
    if (sToken.type != TK_TAGS) {
      return tscInvalidSQLErrMsg(pCmd->payload, "keyword TAGS expected", sql);
    }

    int32_t  numOfTagValues = 0;
    uint32_t ignoreTokenTypes = TK_LP;
    uint32_t numOfIgnoreToken = 1;
    while (1) {
      index = 0;
      sToken = tStrGetToken(sql, &index, true, numOfIgnoreToken, &ignoreTokenTypes);
      sql += index;
      if (sToken.n == 0) {
        break;
      } else if (sToken.type == TK_RP) {
        break;
      }

      // Remove quotation marks
      if (TK_STRING == sToken.type) {
        sToken.z++;
        sToken.n -= 2;
      }

      code = tsParseOneColumnData(&pTagSchema[numOfTagValues], &sToken, tagVal, pCmd->payload, &sql, false,
                                  pMeterMetaInfo->pMeterMeta->precision);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      if ((pTagSchema[numOfTagValues].type == TSDB_DATA_TYPE_BINARY ||
           pTagSchema[numOfTagValues].type == TSDB_DATA_TYPE_NCHAR) && sToken.n > pTagSchema[numOfTagValues].bytes) {
        return tscInvalidSQLErrMsg(pCmd->payload, "string too long", sToken.z);
      }

      tagVal += pTagSchema[numOfTagValues++].bytes;
    }

    if (numOfTagValues != pMeterMetaInfo->pMeterMeta->numOfTags) {
      return tscInvalidSQLErrMsg(pCmd->payload, "number of tags mismatch", sql);
    }

    if (tscValidateName(&tableToken) != TSDB_CODE_SUCCESS) {
      return tscInvalidSQLErrMsg(pCmd->payload, "invalid table name", sql);
    }

    int32_t ret = setMeterID(pSql, &tableToken, 0);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }

    createTable = true;
    code = tscGetMeterMetaEx(pSql, pMeterMetaInfo->name, true);
  } else {
    if (cstart != NULL) {
      sql = cstart;
    } else {
      sql = sToken.z;
    }
    code = tscGetMeterMeta(pSql, pMeterMetaInfo->name, 0);
  }

  int32_t len = cend - cstart + 1;
  if (cstart != NULL && createTable == true) {
    /* move the column list to start position of the next accessed points */
    memmove(sql - len, cstart, len);
    *sqlstr = sql - len;
  } else {
    *sqlstr = sql;
  }

  return code;
}

int validateTableName(char *tblName, int len) {
  char buf[TSDB_METER_ID_LEN] = {0};
  strncpy(buf, tblName, len);

  SSQLToken token = {.n = len, .type = TK_ID, .z = buf};
  tSQLGetToken(buf, &token.type);

  return tscValidateName(&token);
}

/**
 * usage: insert into table1 values() () table2 values()()
 *
 * @param str
 * @param acct
 * @param db
 * @param pSql
 * @return
 */
int doParserInsertSql(SSqlObj *pSql, char *str) {
  SSqlCmd *pCmd = &pSql->cmd;
  
  int32_t code = TSDB_CODE_INVALID_SQL;
  int32_t totalNum = 0;

  SMeterMetaInfo *pMeterMetaInfo = tscAddEmptyMeterMetaInfo(pCmd);

  if ((code = tscAllocPayload(pCmd, TSDB_PAYLOAD_SIZE)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  void *pTableHashList = taosInitIntHash(128, POINTER_BYTES, taosHashInt);

  pSql->cmd.pDataBlocks = tscCreateBlockArrayList();
  tscTrace("%p create data block list for submit data, %p", pSql, pSql->cmd.pDataBlocks);

  while (1) {
    int32_t index = 0;
    SSQLToken sToken = tStrGetToken(str, &index, false, 0, NULL);
    if (sToken.n == 0) { // parse file, do not release the STableDataBlock
      if (pCmd->isInsertFromFile == 1) {
        goto _clean;
      }

      if (totalNum > 0) {
        break;
      } else {  // no data in current sql string, error
        code = TSDB_CODE_INVALID_SQL;
        goto _error_clean;
      }
    }

    // Check if the table name available or not
    if (validateTableName(sToken.z, sToken.n) != TSDB_CODE_SUCCESS) {
      code = tscInvalidSQLErrMsg(pCmd->payload, "table name invalid", sToken.z);
      goto _error_clean;
    }

    //TODO refactor
    if ((code = setMeterID(pSql, &sToken, 0)) != TSDB_CODE_SUCCESS) {
      goto _error_clean;
    }

    void *fp = pSql->fp;
    if ((code = tscParseSqlForCreateTableOnDemand(&str, pSql)) != TSDB_CODE_SUCCESS) {
      if (fp != NULL) {
        goto _clean;
      } else {
        /*
         * for async insert, the free data block operations, which is tscDestroyBlockArrayList,
         * must be executed before launch another threads to get metermeta, since the
         * later ops may manipulate SSqlObj through another thread in getMeterMetaCallback function.
         */
        goto _error_clean;
      }
    }

    if (UTIL_METER_IS_METRIC(pMeterMetaInfo)) {
      code = tscInvalidSQLErrMsg(pCmd->payload, "insert data into super table is not supported", NULL);
      goto _error_clean;
    }

    index = 0;
    sToken = tStrGetToken(str, &index, false, 0, NULL);
    str += index;
    if (sToken.n == 0) {
      code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES or FILE are required", sToken.z);
      goto _error_clean;
    }

    if (sToken.type == TK_VALUES) {
      SParsedDataColInfo spd = {.numOfCols = pMeterMetaInfo->pMeterMeta->numOfColumns};
      SSchema *          pSchema = tsGetSchema(pMeterMetaInfo->pMeterMeta);

      tscSetAssignedColumnInfo(&spd, pSchema, pMeterMetaInfo->pMeterMeta->numOfColumns);

      if (pCmd->isInsertFromFile == -1) {
        pCmd->isInsertFromFile = 0;
      } else {
        if (pCmd->isInsertFromFile == 1) {
          code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES and FILE are not allowed to mix up", sToken.z);
          goto _error_clean;
        }
      }

      /*
       * app here insert data in different vnodes, so we need to set the following
       * data in another submit procedure using async insert routines
       */
      code = doParseInsertStatement(pSql, pTableHashList, &str, &spd, &totalNum);
      if (code != TSDB_CODE_SUCCESS) {
        goto _error_clean;
      }

    } else if (sToken.type == TK_FILE) {
      if (pCmd->isInsertFromFile == -1) {
        pCmd->isInsertFromFile = 1;
      } else {
        if (pCmd->isInsertFromFile == 0) {
          code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES and FILE are not allowed to mix up", sToken.z);
          goto _error_clean;
        }
      }

      index = 0;
      sToken = tStrGetToken(str, &index, false, 0, NULL);
      str += index;
      if (sToken.n == 0) {
        code = tscInvalidSQLErrMsg(pCmd->payload, "file path is required following keyword FILE", sToken.z);
        goto _error_clean;
      }

      char fname[PATH_MAX] = {0};
      strncpy(fname, sToken.z, sToken.n);
      strdequote(fname);

      wordexp_t full_path;
      if (wordexp(fname, &full_path, 0) != 0) {
        code = tscInvalidSQLErrMsg(pCmd->payload, "invalid filename", sToken.z);
        goto _error_clean;
      }
      strcpy(fname, full_path.we_wordv[0]);
      wordfree(&full_path);

      STableDataBlocks *pDataBlock = tscCreateDataBlockEx(PATH_MAX, pMeterMetaInfo->pMeterMeta->rowSize,
                                                          sizeof(SShellSubmitBlock), pMeterMetaInfo->name);

      tscAppendDataBlock(pCmd->pDataBlocks, pDataBlock);
      strcpy(pDataBlock->filename, fname);
    } else if (sToken.type == TK_LP) {
      /* insert into tablename(col1, col2,..., coln) values(v1, v2,... vn); */
      SMeterMeta *pMeterMeta = tscGetMeterMetaInfo(pCmd, 0)->pMeterMeta;
      SSchema *   pSchema = tsGetSchema(pMeterMeta);

      if (pCmd->isInsertFromFile == -1) {
        pCmd->isInsertFromFile = 0;
      } else if (pCmd->isInsertFromFile == 1) {
        code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES and FILE are not allowed to mix up", sToken.z);
        goto _error_clean;
      }

      SParsedDataColInfo spd = {0};
      spd.numOfCols = pMeterMeta->numOfColumns;

      int16_t offset[TSDB_MAX_COLUMNS] = {0};
      for (int32_t t = 1; t < pMeterMeta->numOfColumns; ++t) {
        offset[t] = offset[t - 1] + pSchema[t - 1].bytes;
      }

      while (1) {
        index = 0;
        sToken = tStrGetToken(str, &index, false, 0, NULL);
        str += index;

        if (TK_STRING == sToken.type) {
          sToken.n = strdequote(sToken.z);
          strtrim(sToken.z);
          sToken.n = (uint32_t)strlen(sToken.z);
        }

        if (sToken.type == TK_RP) {
          break;
        }

        bool findColumnIndex = false;

        // todo speedup by using hash list
        for (int32_t t = 0; t < pMeterMeta->numOfColumns; ++t) {
          if (strncmp(sToken.z, pSchema[t].name, sToken.n) == 0 && strlen(pSchema[t].name) == sToken.n) {
            SParsedColElem *pElem = &spd.elems[spd.numOfAssignedCols++];
            pElem->offset = offset[t];
            pElem->colIndex = t;

            if (spd.hasVal[t] == true) {
              code = tscInvalidSQLErrMsg(pCmd->payload, "duplicated column name", sToken.z);
              goto _error_clean;
            }

            spd.hasVal[t] = true;
            findColumnIndex = true;
            break;
          }
        }

        if (!findColumnIndex) {
          code = tscInvalidSQLErrMsg(pCmd->payload, "invalid column name", sToken.z);
          goto _error_clean;
        }
      }

      if (spd.numOfAssignedCols == 0 || spd.numOfAssignedCols > pMeterMeta->numOfColumns) {
        code = tscInvalidSQLErrMsg(pCmd->payload, "column name expected", sToken.z);
        goto _error_clean;
      }

      index = 0;
      sToken = tStrGetToken(str, &index, false, 0, NULL);
      str += index;

      if (sToken.type != TK_VALUES) {
        code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES is expected", sToken.z);
        goto _error_clean;
      }

      code = doParseInsertStatement(pSql, pTableHashList, &str, &spd, &totalNum);
      if (code != TSDB_CODE_SUCCESS) {
        goto _error_clean;
      }
    } else {
      code = tscInvalidSQLErrMsg(pCmd->payload, "keyword VALUES or FILE are required", sToken.z);
      goto _error_clean;
    }
  }

  // we need to keep the data blocks if there are parameters in the sql
  if (pCmd->numOfParams > 0) {
    goto _clean;
  }

  // submit to more than one vnode
  if (pCmd->pDataBlocks->nSize > 0) {
    // merge according to vgid
    if ((code = tscMergeTableDataBlocks(pSql, pCmd->pDataBlocks)) != TSDB_CODE_SUCCESS) {
      goto _error_clean;
    }

    STableDataBlocks *pDataBlock = pCmd->pDataBlocks->pData[0];
    if ((code = tscCopyDataBlockToPayload(pSql, pDataBlock)) != TSDB_CODE_SUCCESS) {
      goto _error_clean;
    }

    SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0);
    
    // set the next sent data vnode index in data block arraylist
    pMeterMetaInfo->vnodeIndex = 1;
  } else {
    pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);
  }

  code = TSDB_CODE_SUCCESS;
  goto _clean;

_error_clean:
  pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);

_clean:
  taosCleanUpIntHash(pTableHashList);
  return code;
}

int tsParseInsertSql(SSqlObj *pSql, char *sql, char *acct, char *db) {
  if (!pSql->pTscObj->writeAuth) {
    return TSDB_CODE_NO_RIGHTS;
  }

  int32_t  index = 0;
  SSqlCmd *pCmd = &pSql->cmd;

  SSQLToken sToken = tStrGetToken(sql, &index, false, 0, NULL);
  
  assert(sToken.type == TK_INSERT || sToken.type == TK_IMPORT);
  pCmd->import = (sToken.type == TK_IMPORT);
  
  sToken = tStrGetToken(sql, &index, false, 0, NULL);
  if (sToken.type != TK_INTO) {
    return tscInvalidSQLErrMsg(pCmd->payload, "keyword INTO is expected", sToken.z);
  }
  
  pCmd->count = 0;
  pCmd->command = TSDB_SQL_INSERT;
  pCmd->isInsertFromFile = -1;
  pSql->res.numOfRows = 0;
  
  return doParserInsertSql(pSql, sql + index);
}

int tsParseSql(SSqlObj *pSql, char *acct, char *db, bool multiVnodeInsertion) {
  int32_t ret = TSDB_CODE_SUCCESS;

  // must before clean the sqlcmd object
  tscRemoveAllMeterMetaInfo(&pSql->cmd, false);
  tscCleanSqlCmd(&pSql->cmd);

  if (tscIsInsertOrImportData(pSql->sqlstr)) {
    /*
     * only for async multi-vnode insertion
     * Set the fp before parse the sql string, in case of getmetermeta failed, in which
     * the error handle callback function can rightfully restore the user defined function (fp)
     */
    if (pSql->fp != NULL && multiVnodeInsertion) {
      assert(pSql->fetchFp == NULL);
      pSql->fetchFp = pSql->fp;

      // replace user defined callback function with multi-insert proxy function
      pSql->fp = tscAsyncInsertMultiVnodesProxy;
    }

    ret = tsParseInsertSql(pSql, pSql->sqlstr, acct, db);
  } else {
    ret = tscAllocPayload(&pSql->cmd, TSDB_DEFAULT_PAYLOAD_SIZE);
    if (TSDB_CODE_SUCCESS != ret) return ret;
    
    SSqlInfo SQLInfo = {0};
    tSQLParse(&SQLInfo, pSql->sqlstr);

    ret = tscToSQLCmd(pSql, &SQLInfo);
    SQLInfoDestroy(&SQLInfo);
  }

  /*
   * the pRes->code may be modified or even released by another thread in tscMeterMetaCallBack
   * function, so do NOT use pRes->code to determine if the getMeterMeta/getMetricMeta function
   * invokes new threads to get data from mnode or simply retrieves data from cache.
   *
   * do NOT assign return code to pRes->code for the same reason for it may be released by another thread
   * pRes->code = ret;
   */
  return ret;
}

static int doPackSendDataBlock(SSqlObj *pSql, int32_t numOfRows, STableDataBlocks *pTableDataBlocks) {
  int32_t  code = TSDB_CODE_SUCCESS;
  SSqlCmd *pCmd = &pSql->cmd;

  SMeterMeta *pMeterMeta = tscGetMeterMetaInfo(pCmd, 0)->pMeterMeta;

  SShellSubmitBlock *pBlocks = (SShellSubmitBlock *)(pTableDataBlocks->pData);
  tsSetBlockInfo(pBlocks, pMeterMeta, numOfRows);

  if ((code = tscMergeTableDataBlocks(pSql, pCmd->pDataBlocks)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  // the pDataBlock is different from the pTableDataBlocks
  STableDataBlocks *pDataBlock = pCmd->pDataBlocks->pData[0];
  if ((code = tscCopyDataBlockToPayload(pSql, pDataBlock)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  if ((code = tscProcessSql(pSql)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  return TSDB_CODE_SUCCESS;
}

static int tscInsertDataFromFile(SSqlObj *pSql, FILE *fp, char *tmpTokenBuf) {
  size_t          readLen = 0;
  char *          line = NULL;
  size_t          n = 0;
  int             len = 0;
  uint32_t        maxRows = 0;
  SSqlCmd *       pCmd = &pSql->cmd;
  int             numOfRows = 0;
  int32_t         code = 0;
  int             nrows = 0;
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0);
  SMeterMeta *    pMeterMeta = pMeterMetaInfo->pMeterMeta;
  int32_t         rowSize = pMeterMeta->rowSize;

  pCmd->pDataBlocks = tscCreateBlockArrayList();
  STableDataBlocks *pTableDataBlock =
      tscCreateDataBlockEx(TSDB_PAYLOAD_SIZE, pMeterMeta->rowSize, sizeof(SShellSubmitBlock), pMeterMetaInfo->name);

  tscAppendDataBlock(pCmd->pDataBlocks, pTableDataBlock);

  maxRows = tscAllocateMemIfNeed(pTableDataBlock, rowSize);
  if (maxRows < 1) return -1;

  int                count = 0;
  SParsedDataColInfo spd = {.numOfCols = pMeterMetaInfo->pMeterMeta->numOfColumns};
  SSchema *          pSchema = tsGetSchema(pMeterMetaInfo->pMeterMeta);

  tscSetAssignedColumnInfo(&spd, pSchema, pMeterMetaInfo->pMeterMeta->numOfColumns);

  while ((readLen = getline(&line, &n, fp)) != -1) {
    // line[--readLen] = '\0';
    if (('\r' == line[readLen - 1]) || ('\n' == line[readLen - 1])) line[--readLen] = 0;
    if (readLen == 0) continue; //fang, <= to ==

    char *lineptr = line;
    strtolower(line, line);

    if (numOfRows >= maxRows || pTableDataBlock->size + pMeterMeta->rowSize >= pTableDataBlock->nAllocSize) {
      uint32_t tSize = tscAllocateMemIfNeed(pTableDataBlock, pMeterMeta->rowSize);
      if (0 == tSize) return (-TSDB_CODE_CLI_OUT_OF_MEMORY);
      maxRows += tSize;    
    }

    len = tsParseOneRowData(&lineptr, pTableDataBlock, pSchema, &spd, pCmd->payload, pMeterMeta->precision, &code, tmpTokenBuf);
    if (len <= 0 || pTableDataBlock->numOfParams > 0) {
      pSql->res.code = code;
      return (-code);
    }
    
    pTableDataBlock->size += len;

    count++;
    nrows++;
    if (count >= maxRows) {
      if ((code = doPackSendDataBlock(pSql, count, pTableDataBlock)) != TSDB_CODE_SUCCESS) {
        return -code;
      }

      pTableDataBlock = pCmd->pDataBlocks->pData[0];
      pTableDataBlock->size = sizeof(SShellSubmitBlock);
      pTableDataBlock->rowSize = pMeterMeta->rowSize;

      numOfRows += pSql->res.numOfRows;
      pSql->res.numOfRows = 0;
      count = 0;
    }
  }

  if (count > 0) {
    if ((code = doPackSendDataBlock(pSql, count, pTableDataBlock)) != TSDB_CODE_SUCCESS) {
      return -code;
    }

    numOfRows += pSql->res.numOfRows;
    pSql->res.numOfRows = 0;
  }

  if (line) tfree(line);

  return numOfRows;
}

/* multi-vnodes insertion in sync query model
 *
 * modify history
 * 2019.05.10 lihui
 * Remove the code for importing records from files
 */
void tscProcessMultiVnodesInsert(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;

  // not insert/import, return directly
  if (pCmd->command != TSDB_SQL_INSERT) {
    return;
  }

  // SSqlCmd may have been released
  if (pCmd->pDataBlocks == NULL) {
    return;
  }

  STableDataBlocks *pDataBlock = NULL;
  SMeterMetaInfo *  pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0);
  int32_t           code = TSDB_CODE_SUCCESS;

  /* the first block has been sent to server in processSQL function */
  assert(pCmd->isInsertFromFile != -1 && pMeterMetaInfo->vnodeIndex >= 1 && pCmd->pDataBlocks != NULL);

  if (pMeterMetaInfo->vnodeIndex < pCmd->pDataBlocks->nSize) {
    SDataBlockList *pDataBlocks = pCmd->pDataBlocks;

    for (int32_t i = pMeterMetaInfo->vnodeIndex; i < pDataBlocks->nSize; ++i) {
      pDataBlock = pDataBlocks->pData[i];
      if (pDataBlock == NULL) {
        continue;
      }

      if ((code = tscCopyDataBlockToPayload(pSql, pDataBlock)) != TSDB_CODE_SUCCESS) {
        tscTrace("%p build submit data block failed, vnodeIdx:%d, total:%d", pSql, pMeterMetaInfo->vnodeIndex, pDataBlocks->nSize);
        continue;
      }

      tscProcessSql(pSql);
    }
  }

  // all data have been submit to vnode, release data blocks
  pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);
}

// multi-vnodes insertion in sync query model
void tscProcessMultiVnodesInsertForFile(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  if (pCmd->command != TSDB_SQL_INSERT) {
    return;
  }

  SMeterMetaInfo *  pInfo = tscGetMeterMetaInfo(pCmd, 0);
  STableDataBlocks *pDataBlock = NULL;
  int32_t           affected_rows = 0;

  assert(pCmd->isInsertFromFile == 1 && pCmd->pDataBlocks != NULL);
  SDataBlockList *pDataBlockList = pCmd->pDataBlocks;
  pCmd->pDataBlocks = NULL;

  char path[PATH_MAX] = {0};

  for (int32_t i = 0; i < pDataBlockList->nSize; ++i) {
    pDataBlock = pDataBlockList->pData[i];
    if (pDataBlock == NULL) {
      continue;
    }
    
    if (TSDB_CODE_SUCCESS != tscAllocPayload(pCmd, TSDB_PAYLOAD_SIZE)) {
      tscError("%p failed to malloc when insert file", pSql);
      continue;
    }
    pCmd->count = 1;

    strncpy(path, pDataBlock->filename, PATH_MAX);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
      tscError("%p failed to open file %s to load data from file, reason:%s", pSql, path, strerror(errno));
      continue;
    }

    strncpy(pInfo->name, pDataBlock->meterId, TSDB_METER_ID_LEN);
    memset(pDataBlock->pData, 0, pDataBlock->nAllocSize);

    int32_t ret = tscGetMeterMeta(pSql, pInfo->name, 0);
    if (ret != TSDB_CODE_SUCCESS) {
      tscError("%p get meter meta failed, abort", pSql);
      continue;
    }
    
    char*   tmpTokenBuf = calloc(1, 4096);  // used for deleting Escape character: \\, \', \"
    if (NULL == tmpTokenBuf) {
      tscError("%p calloc failed", pSql);
      continue;
    }

    int nrows = tscInsertDataFromFile(pSql, fp, tmpTokenBuf);
    free(tmpTokenBuf);
    
    pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);

    if (nrows < 0) {
      fclose(fp);
      tscTrace("%p no records(%d) in file %s", pSql, nrows, path);
      continue;
    }

    fclose(fp);
    affected_rows += nrows;

    tscTrace("%p Insert data %d records from file %s", pSql, nrows, path);
  }

  pSql->res.numOfRows = affected_rows;

  // all data have been submit to vnode, release data blocks
  pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);
  tscDestroyBlockArrayList(pDataBlockList);
}
