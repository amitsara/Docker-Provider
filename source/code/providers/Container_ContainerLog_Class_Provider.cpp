/* @migen@ */
#include <MI.h>
#include "Container_ContainerLog_Class_Provider.h"

#include <map>
#include <stdio.h>
#include <string>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <vector>

#include "../cjson/cJSON.h"
#include "../dockerapi/DockerRemoteApi.h"
#include "../dockerapi/DockerRestHelper.h"

#define LASTLOGQUERYTIMEFILE "/var/opt/microsoft/docker-cimprov/state/LastLogQueryTime.txt"

using namespace std;

class Guid
{
public:
    ///
    /// Create a guid and represent it as string
    ///
    static string NewToString()
    {
        uuid_t uuid;
        uuid_generate_random(uuid);
        char s[37];
        uuid_unparse(uuid, s);
        return string(s);
    }
};

MI_BEGIN_NAMESPACE

class ContainerLogQuery
{
private:
    ///
    /// Get the previous time if it was stored, otherwise get the current time
    ///
    /// \returns Time of previous query if available
    ///
    static int GetPreviousTime()
    {
        int fileTime = time(NULL);
        int currentTime = fileTime;
        const char* lastQueryFile = LASTLOGQUERYTIMEFILE;
        FILE* file = fopen(lastQueryFile, "r");

        if (file)
        {
            fscanf(file, "%d", &fileTime);
            fclose(file);

            if (fileTime > currentTime)
            {
                syslog(LOG_WARNING, "The time stored in %s is more recent than the current time", lastQueryFile);
            }
        }
        else
        {
            syslog(LOG_ERR, "Attempt in GetPreviousTime to open %s for reading failed", lastQueryFile);
        }

        // Discard stored times that are more recent than the current time
        return fileTime > currentTime ? currentTime : fileTime;
    }

    ///
    /// Write the previous time to disk
    ///
    /// \param[in] t The time
    ///
    static void SetPreviousTime(int t)
    {
        const char* lastQueryFile = LASTLOGQUERYTIMEFILE;
        FILE* file = fopen(lastQueryFile, "w");

        if (file)
        {
            fprintf(file, "%d", t);
            fclose(file);
        }
        else
        {
            syslog(LOG_ERR, "Attempt in SetPreviousTime to open %s for writing failed", lastQueryFile);
        }
    }

    ///
    /// Get the logging driver
    ///
    static string getLogDriverName()
    {
        string logDriverName;

        vector<string> request(1, DockerRestHelper::restDockerInfo());
        vector<cJSON*> response = getResponse(request);

        if (!response.empty() && response[0])
        {
            logDriverName = string(cJSON_GetObjectItem(response[0], "LoggingDriver")->valuestring);
            cJSON_Delete(response[0]);
        }

        return logDriverName;
    }

public:
    ///
    /// Get logs for all containers on the host
    ///
    /// \returns Vector containing logs for each container
    ///
    static vector<Container_ContainerLog_Class> QueryAll()
    {
        vector<Container_ContainerLog_Class> result;

        string logDriver = getLogDriverName();
        if (logDriver.compare("json-file") != 0)
        {
            return result;
        }

        int previousTime = GetPreviousTime();
        int currentTime = time(NULL);

        // Get computer name
        string hostname = getDockerHostName();

        // Request containers
        vector<string> request(1, DockerRestHelper::restDockerPs());
        vector<cJSON*> response = getResponse(request);

        // See http://docs.docker.com/reference/api/Container_remote_api_v1.21/#list-containers for example output
        if (!response.empty() && response[0])
        {
            for (int i = 0; i < cJSON_GetArraySize(response[0]); i++)
            {
                cJSON* entry = cJSON_GetArrayItem(response[0], i);
                if (entry)
                {
                    string containerId = string(cJSON_GetObjectItem(entry, "Id")->valuestring);
                    string imageName = string(cJSON_GetObjectItem(entry, "Image")->valuestring);
                    string containerName;

                    // Get container name
                    cJSON* names = cJSON_GetObjectItem(entry, "Names");
                    if (cJSON_GetArraySize(names))
                    {
                        containerName = string(cJSON_GetArrayItem(names, 0)->valuestring + 1);
                    }

                    // Get container logs
                    string logRequest = DockerRestHelper::restDockerLogs(containerId, previousTime);
                    vector<string> logResponse = getContainerLogs(logRequest);

                    // See http://docs.docker.com/reference/api/Container_remote_api_v1.21/#list-containers for example output
                    if (!logResponse.empty())
                    {
                        for (int j = 0; j < (int)logResponse.size(); j++)
                        {
                            Container_ContainerLog_Class instance;
                            instance.InstanceID_value(Guid::NewToString().c_str());
                            instance.Image_value(imageName.c_str());
                            instance.ImageName_value(imageName.c_str());
                            instance.Id_value(containerId.c_str());
                            instance.Name_value(containerName.c_str());

                            // split the message. 
                            // message format : stdout;log
                            string message = logResponse[j];
                            std::size_t pos = message.find(';');
                            if(pos != std::string::npos)
                            {
                                instance.LogEntrySource_value(message.substr(0, pos).c_str());
                                if(pos + 1 != std::string::npos)
                                {
                                    instance.LogEntry_value(message.substr(pos + 1).c_str());
                                }
                            }
                            instance.Computer_value(hostname.c_str());
                            result.push_back(instance);
                        }

                        logResponse.clear();
                    }
                }
                else
                {
                    syslog(LOG_WARNING, "API call in Container_ContainerLog::QueryAll to inspect container returned null");
                }
            }

            // Clean up object
            cJSON_Delete(response[0]);
        }
        else
        {
            syslog(LOG_WARNING, "API call in Container_ContainerLog::QueryAll to list containers returned null");
        }

        SetPreviousTime(currentTime);
        return result;
    }
};

Container_ContainerLog_Class_Provider::Container_ContainerLog_Class_Provider(
    Module* module) :
    m_Module(module)
{
}

Container_ContainerLog_Class_Provider::~Container_ContainerLog_Class_Provider()
{
}

void Container_ContainerLog_Class_Provider::Load(
        Context& context)
{
    context.Post(MI_RESULT_OK);
}

void Container_ContainerLog_Class_Provider::Unload(
        Context& context)
{
    context.Post(MI_RESULT_OK);
}

void Container_ContainerLog_Class_Provider::EnumerateInstances(
    Context& context,
    const String& nameSpace,
    const PropertySet& propertySet,
    bool keysOnly,
    const MI_Filter* filter)
{
    vector<Container_ContainerLog_Class> queryResult = ContainerLogQuery::QueryAll();

    for (unsigned i = 0; i < queryResult.size(); i++)
    {
        context.Post(queryResult[i]);
    }

    context.Post(MI_RESULT_OK);
}

void Container_ContainerLog_Class_Provider::GetInstance(
    Context& context,
    const String& nameSpace,
    const Container_ContainerLog_Class& instanceName,
    const PropertySet& propertySet)
{
    context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerLog_Class_Provider::CreateInstance(
    Context& context,
    const String& nameSpace,
    const Container_ContainerLog_Class& newInstance)
{
    context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerLog_Class_Provider::ModifyInstance(
    Context& context,
    const String& nameSpace,
    const Container_ContainerLog_Class& modifiedInstance,
    const PropertySet& propertySet)
{
    context.Post(MI_RESULT_NOT_SUPPORTED);
}

void Container_ContainerLog_Class_Provider::DeleteInstance(
    Context& context,
    const String& nameSpace,
    const Container_ContainerLog_Class& instanceName)
{
    context.Post(MI_RESULT_NOT_SUPPORTED);
}


MI_END_NAMESPACE
