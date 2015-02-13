/*
 * ===========================================================================
 * Loom SDK
 * Copyright 2011, 2012, 2013
 * The Game Engine Company, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ===========================================================================
 */

#include "platform/CCFileUtils.h"
#include "loom/script/loomscript.h"
#include "loom/script/runtime/lsRuntime.h"
#include "loom/common/core/log.h"
#include "loom/script/native/lsNativeDelegate.h"
#include "loom/vendor/sqlite3/sqlite3.h"
#include "loom/common/utils/utTypes.h"
#include "loom/common/utils/utByteArray.h"
#include "loom/common/utils/utString.h"
#include "loom/common/platform/platformThread.h"
#include "loom/script/native/core/system/lmJSON.h"


lmDefineLogGroup(gSQLiteGroup, "loom.sqlite", 1, LoomLogInfo);

using namespace LS;



//forward declaration of Statement
class Statement;


//SQLite Connection binding for Loomscript
class Connection
{
protected:
    utString databaseName;
    utString databaseFullPath;

public:
    LOOM_STATICDELEGATE(OnImportComplete);

    sqlite3 *dbHandle;

    static bool backgroundImportInProgress;
    static MutexHandle backgroundImportMutex;
    static const char *backgroundImportDatabase;
    static JSON *backgroundImportData;

    static Connection *open(const char *database, int flags);
    static bool backgroundImport(const char *database, JSON *data);
    static const char *getVersion();
    static void backgroundImportDone(int result);
    static int __stdcall backgroundImportBody(void *param);

    Statement *prepare(const char *query);
    const char* getDBName() { return databaseName.c_str(); }



    int getErrorCode()
    {
        return sqlite3_errcode(dbHandle);
    }
  
    const char* getErrorMessage()
    {
        return sqlite3_errmsg(dbHandle);
    }
      
    int getlastInsertRowId()
    {
        //"NOTE: In SQLite the row ID is a 64-bit integer but for all practical 
        //database sizes you can cast the 64 bit value to a 32-bit integer."
        //
        // - Some Guy on The Internet
        //
        sqlite3_int64 rowid64 = sqlite3_last_insert_rowid(dbHandle);

        //safety check on the value of the row as SQLite allows 64bit ints and Loomscript 
        //only supports 32bit (31 for signed)
        if(rowid64 > (2^31))
        {
            lmLogError(gSQLiteGroup, "RowID found in getlastInsertRowId the SQLite database %s is larger than a 32 bit integer! The return value will not be as expected!", getDBName());
        }
        return (int)(rowid64 & 0x00000000ffffffff);
    }

    int close()
    {
        //close the database
        int result = sqlite3_close_v2(dbHandle);
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error closing the SQLite database: %s with message: %s", getDBName(), getErrorMessage());
        }
        return result;
    }
};



//SQLite Statement binding for Loomscript
class Statement
{
public:
    LOOM_DELEGATE(OnStatementProgress);
    LOOM_DELEGATE(OnStatementComplete);

    bool asyncStepInProgress;
    MutexHandle stepAsyncMutex;
    Connection *parentDB;
    sqlite3_stmt *statementHandle;

    static int statementProgressVMIWait;
    static int stepAsyncProgress(void *param);
    static int __stdcall stepAsyncBody(void *param);


    Statement(Connection *c)
    {
        parentDB = c;
        asyncStepInProgress = false;

        //create our mutex now 
        stepAsyncMutex = loom_mutex_create();
    }

    ~Statement()
    {
        loom_mutex_destroy(stepAsyncMutex);
    }

    int getParameterCount()
    {
        return sqlite3_bind_parameter_count(statementHandle);
    }

    const char *getParameterName(int index)
    {
        const char *name = sqlite3_bind_parameter_name(statementHandle, index);
        if(name == NULL)
        {
            lmLogError(gSQLiteGroup, "Invalid index for getParameterName in database: %s", parentDB->getDBName());
        }
        return name;
    }

    int getParameterIndex(const char* name)
    {
        int index = sqlite3_bind_parameter_index(statementHandle, name);
        if(name == NULL)
        {
            lmLogError(gSQLiteGroup, "Invalid name for getParameterIndex in database: %s", parentDB->getDBName());
        }
        return index;
    }

    int bindInt(int index, int value)
    {
        int result = sqlite3_bind_int(statementHandle, index, value);
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling bindInt for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;        
    }

    int bindDouble(int index, double value)
    {
        int result = sqlite3_bind_double(statementHandle, index, value);
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling bindDouble for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;        
    }

    int bindString(int index, const char *value)
    {
        int result = sqlite3_bind_text(statementHandle, index, value, -1, SQLITE_TRANSIENT); 
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling bindString for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;        
    }

    int bindBytes(int index, utByteArray *value)
    {
        void *bytes;
        int size;

        if(!value || !value->getSize())
        {
            bytes = NULL;
            size = 0;
        }
        else
        {
            bytes = value->getDataPtr();      
            size = (int)value->getSize();
        }

        //bind the blob to the statement
        int result = sqlite3_bind_blob(statementHandle, index, (const void *)bytes, size, SQLITE_TRANSIENT); 
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling bindBytes for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;
    }

    int step()
    {
        int result = sqlite3_step(statementHandle); 
        if(result == SQLITE_ERROR)
        {
            lmLogError(gSQLiteGroup, "Error calling step for database: %s", parentDB->getDBName());
        }
        return result;
    }

    bool stepAsync()
    {
        if(asyncStepInProgress)
        {
            lmLogError(gSQLiteGroup, "Attempting to run multiple stepAsync calls on the same statement for database: %s", parentDB->getDBName());
            return false;
        }

        //set up the progress handler
        sqlite3_progress_handler(parentDB->dbHandle, 
                                    Statement::statementProgressVMIWait, 
                                    Statement::stepAsyncProgress, 
                                    this);

        //start up the stepAsync thread
        asyncStepInProgress = true;
        loom_thread_start(Statement::stepAsyncBody, (void *)this);    
        return true;
    }

    const char *columnName(int col)
    {
        return sqlite3_column_name(statementHandle, col);
    }

    int columnType(int col)
    {
        return sqlite3_column_type(statementHandle, col);
    }

    int columnInt(int col)
    {
        return sqlite3_column_int(statementHandle, col);
    }

    double columnDouble(int col)
    {
        return sqlite3_column_double(statementHandle, col);
    }

    const char* columnString(int col)
    {
        return (const char *)sqlite3_column_text(statementHandle, col);
    }

    utByteArray *columnBytes(int col)
    {
        //get the blob from the column
        void *blob = (void *)sqlite3_column_blob(statementHandle, col);
        if(blob == NULL)
        {
            return NULL;
        }
        int size = sqlite3_column_bytes(statementHandle, col);

        //valid blob so allocate byte array for it
        utByteArray *bytes = new utByteArray();
        bytes->allocateAndCopy(blob, size);     

        return bytes;   
    }

    int reset()
    {
        int result = sqlite3_reset(statementHandle); 
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling reset for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;        
    }

    int finalize()
    {
        int result = sqlite3_finalize(statementHandle); 
        if(result != SQLITE_OK)
        {
            lmLogError(gSQLiteGroup, "Error calling finalize for database: %s with Result Code: %i", parentDB->getDBName(), result);
        }
        return result;        
    }
};




//---Statement--- external function initialisation
int Statement::statementProgressVMIWait = 1;

int Statement::stepAsyncProgress(void *param)
{
    //get the statement
    Statement *s = (Statement *)param;

    //call our progress delegate
    s->_OnStatementProgressDelegate.invoke();    
}

int __stdcall Statement::stepAsyncBody(void *param)
{
    //get the statement
    Statement *s = (Statement *)param;

    //call the internal SQLite step function, then yield the thread
    int result = sqlite3_step(s->statementHandle); 

    //NOTE: shouldn't need to yield the thread here as the thread will die now anyways...
    // loom_thread_yield();

    //fire our completion delegate with the result
    s->_OnStatementCompleteDelegate.pushArgument(result);
    s->_OnStatementCompleteDelegate.invoke();

    //we're done now, so need to turn off our processing flag
    loom_mutex_lock(s->stepAsyncMutex);
    s->asyncStepInProgress = false;
    loom_mutex_unlock(s->stepAsyncMutex);

    return 0;
}



//---Connection--- external function / variable initialisation
NativeDelegate Connection::_OnImportCompleteDelegate;
bool Connection::backgroundImportInProgress = false;
MutexHandle Connection::backgroundImportMutex = loom_mutex_create();;
const char *Connection::backgroundImportDatabase = NULL;
JSON *Connection::backgroundImportData = NULL;

Statement *Connection::prepare(const char *query)
{
    Statement *s = new Statement(this);

    //prepare the database with the query provided
    int res = sqlite3_prepare_v2(dbHandle, query, -1, &s->statementHandle, NULL);
    if(res != SQLITE_OK)
    {
        lmLogError(gSQLiteGroup, "Error preparing the SQLite database: %s with message: %s", getDBName(), getErrorMessage());
    }
    return s;
}

Connection *Connection::open(const char *database, int flags)
{
    Connection *c;

    //check for valid database name
    if((database == NULL) || (database[0] == '\0'))
    {
        lmLogError(gSQLiteGroup, "Invalid database name specified!");
        return NULL;
    }

    //create the connection
    c = new Connection();
    c->databaseName = utString(database);

    //if we find a path separator in the database name, we assume that it contains a valid path already
    if(strchr(database, '/') || strchr(database, '\\'))
    {
        c->databaseFullPath = c->databaseName;
    }
    else
    {
        //no separator, so we need to prefix the system writable path in front of the database name
        c->databaseFullPath = utString(cocos2d::CCFileUtils::sharedFileUtils()->getWriteablePath().c_str()) + c->databaseName;
    }
    
    //open the SQLite DB
    int res = sqlite3_open_v2(c->databaseFullPath.c_str(), &c->dbHandle, flags, 0);
    if(res != SQLITE_OK)
    {
        lmLogError(gSQLiteGroup, "Error opening the SQLite database file: %s with message: %s", c->databaseFullPath.c_str(), c->getErrorMessage());
    }
    return c;
}

bool Connection::backgroundImport(const char *database, JSON *data)
{
    if(Connection::backgroundImportInProgress)
    {
        lmLogError(gSQLiteGroup, "Attempting to run multiple backgroundImport calls on the same database: %s", database);
        return false;
    }

    //store values to be used in the thread
    Connection::backgroundImportDatabase = database;
    Connection::backgroundImportData = data;

    //start up the backgroundImport thread
    Connection::backgroundImportInProgress = true;
    loom_thread_start(Connection::backgroundImportBody, NULL);    
    return true;

}

const char* Connection::getVersion()
{
    return sqlite3_libversion();
}

void Connection::backgroundImportDone(int result)
{    
    //fire our completion delegate
    Connection::_OnImportCompleteDelegate.pushArgument(result);
    Connection::_OnImportCompleteDelegate.invoke();

    //we're done now, so need to turn off our processing flag
    loom_mutex_lock(Connection::backgroundImportMutex);
    Connection::backgroundImportInProgress = false;
    loom_mutex_unlock(Connection::backgroundImportMutex);
}

int __stdcall Connection::backgroundImportBody(void *param)
{
    int i;
    int arraySize;
    const char *database;
    JSON *data;
    JSON *dataArray;
    JSON *valueObject;
    Connection *c;
    const char *tableName;
    const char *valueKey;
    utString query;


    //get the database name and JSON data
    database = Connection::backgroundImportDatabase;
    data = Connection::backgroundImportData;

//BEN QUESTION: Do we need to handle CREATE of a database as well? 
//    Or will backgroundImport only be used on already created DBs?

    //open the database for read/write
    c = Connection::open(database, SQLITE_OPEN_READWRITE);
    if((c == NULL) || (c->getErrorCode() != SQLITE_OK))
    {
        if(c != NULL)
        {
            c->close();
        }
        Connection::backgroundImportDone(c->getErrorCode());        
        return 0;
    }
    //NOTE: shouldn't need to yield the thread here as the thread will die now anyways...
    // loom_thread_yield();


////////////////////////////////////////////////
//
//BEN QUESTION: Does this functionality seem to be what you want?
//
    //pull out the JSON data to use in the query
    //NOTE: this expects the JSON to be formatted as a single key:array[object,object,...] like:
    //      { "table_name": [ {"column": value}, ... ] }
    tableName = data->getObjectFirstKey();
    dataArray = data->getArray(tableName);
    arraySize = dataArray->getArrayCount();

    //create the INSERT query statement to be something like "INSERT into my_table values(?,?,?)"
    query = utString("INSERT into ") + utString(tableName) + utString(" values (");
    for(i=0;i<arraySize;i++)
    {
        if(i != 0)
        {
            query += utString(",");
        }
        query += utString("?");
    }
    query += utString(")");

    //create the Statement to bind the data to
    Statement *s = c->prepare(query.c_str());
    if(c->getErrorCode() != SQLITE_OK)
    {
        c->close();
        Connection::backgroundImportDone(c->getErrorCode());        
        return 0;        
    }

    //go through the data array and bind all of the values to the statement
    for(i=0;i<arraySize;i++)
    {
        valueObject = dataArray->getArrayObject(i);
        valueKey = valueObject->getObjectFirstKey();
        switch(valueObject->getObjectJSONType(valueKey))
        {
            case JSON_STRING:
                s->bindString(i + 1, valueObject->getString(valueKey));
                break;
            case JSON_INTEGER:
                s->bindInt(i + 1, valueObject->getInteger(valueKey));
                break;
            case JSON_REAL:
                s->bindDouble(i + 1, valueObject->getFloat(valueKey));
                break;
            case JSON_TRUE:
            case JSON_FALSE:
                s->bindInt(i + 1, (valueObject->getBoolean(valueKey) ? 1 : 0));
                break;
        }
    }
    s->step();
    s->finalize();
////////////////////////////////////////////////////////////

    //done, so close the connection now
    c->close();
    Connection::backgroundImportDone(SQLITE_OK);        

    return 0;
}


//Loomscript binding registations for Connection and Statement
static int registerLoomSQLiteConnection(lua_State *L)
{
    beginPackage(L, "loom.sqlite")
      .beginClass<Connection>("Connection")

        .addStaticProperty("onImportComplete", &Connection::getOnImportCompleteDelegate)

        .addStaticMethod("open", &Connection::open)
        .addStaticMethod("backgroundImport", &Connection::backgroundImport)
        .addStaticMethod("__pget_version", &Connection::getVersion)

        .addMethod("__pget_errorCode", &Connection::getErrorCode)
        .addMethod("__pget_errorMessage", &Connection::getErrorMessage)
        .addMethod("__pget_lastInsertRowId", &Connection::getlastInsertRowId)
        .addMethod("prepare", &Connection::prepare)
        .addMethod("close", &Connection::close)

      .endClass()
    .endPackage();
    return 0;
}


static int registerLoomSQLiteStatement(lua_State *L)
{
    beginPackage(L, "loom.sqlite")
      .beginClass<Statement>("Statement")

        .addStaticVar("statementProgressVMIWait", &Statement::statementProgressVMIWait)
        .addVarAccessor("onStatementProgress", &Statement::getOnStatementProgressDelegate)
        .addVarAccessor("onStatementComplete", &Statement::getOnStatementCompleteDelegate)

        .addMethod("getParameterCount", &Statement::getParameterCount)
        .addMethod("getParameterName", &Statement::getParameterName)
        .addMethod("getParameterIndex", &Statement::getParameterIndex)
        .addMethod("bindInt", &Statement::bindInt)
        .addMethod("bindDouble", &Statement::bindDouble)
        .addMethod("bindString", &Statement::bindString)
        .addMethod("bindBytes", &Statement::bindBytes)
        .addMethod("step", &Statement::step)
        .addMethod("stepAsync", &Statement::stepAsync)
        .addMethod("columnName", &Statement::columnName)
        .addMethod("columnType", &Statement::columnType)
        .addMethod("columnInt", &Statement::columnInt)
        .addMethod("columnDouble", &Statement::columnDouble)
        .addMethod("columnString", &Statement::columnString)
        .addMethod("columnBytes", &Statement::columnBytes)
        .addMethod("reset", &Statement::reset)
        .addMethod("finalize", &Statement::finalize)

      .endClass()
    .endPackage();
    return 0;
}


void installLoomSQLite()
{
    LOOM_DECLARE_NATIVETYPE(Connection, registerLoomSQLiteConnection);
    LOOM_DECLARE_NATIVETYPE(Statement, registerLoomSQLiteStatement);
}
