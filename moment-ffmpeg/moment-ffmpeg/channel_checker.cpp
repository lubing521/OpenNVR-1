#include <moment-ffmpeg/inc.h>

#include <moment-ffmpeg/channel_checker.h>
#include <moment-ffmpeg/media_reader.h>
#include <moment-ffmpeg/time_checker.h>
#include <moment-ffmpeg/naming_scheme.h>

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif


using namespace M;
using namespace Moment;

namespace MomentFFmpeg {

#define CONCAT_INTERVAL 6
#define TIMER_TICK 5 // in seconds

static LogGroup libMary_logGroup_channelcheck ("mod_ffmpeg.channelcheck", LogLevel::E);

#ifdef __linux__
uint64_t get_dir_space(char *dirname)
{
  DIR *dir;
  struct dirent *ent;
  struct stat st;
  char buf[PATH_MAX];
  uint64_t totalsize = 0LL;

  if(!(dir = opendir(dirname)))
  {
    logE(channelcheck, _func_, "fail to open folder: ", dirname);
    return totalsize;
  }

  while((ent = readdir(dir)))
  {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;

    sprintf(buf, "%s%s", dirname, ent->d_name);

    if(lstat(buf, &st) == -1)
    {
      logE(channelcheck, _func_, "Couldn't stat: ", buf);
      continue;
    }

    if(S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
    {
      uint64_t dirsize;

      strcat(buf, "/");
      dirsize = get_dir_space(buf);
      totalsize += dirsize;
    }
    else if(S_ISREG(st.st_mode)) // We only want to count regular files
    {
      totalsize += st.st_size;
    }
  }

  closedir(dir);
  return totalsize;
}
#endif

bool ChannelChecker::writeIdx(const ChannelFileSummary & files_existence,
                              StRef<String> const dir_name, StRef<String> const channel_name)
{
    TimeChecker tc;tc.Start();

    bool bRes = false;

    StRef<String> const idx_file = st_makeString (dir_name, "/", channel_name, "/", channel_name, ".idx");
    std::string path = idx_file->cstr();

    std::ofstream idxFile;
    idxFile.open(path);
    if (idxFile.is_open())
    {
        ChannelFileSummary::const_iterator it = files_existence.begin();
        
        for(; it != files_existence.end(); ++it)
        {
            idxFile << it->first;
            idxFile << "|";
            idxFile << it->second.timeStart;
            idxFile << "|";
            idxFile << it->second.timeEnd;
            idxFile << std::endl;
        }
        idxFile.close();
        logD(channelcheck, _func_, "write successful, idxfile: ", path.c_str());
        bRes = true;
    }
    else
    {
        logD(channelcheck, _func_, "fail to open idxFile: ", path.c_str());
        bRes = false;
    }

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"ChannelChecker.writeIdx exectime = [", t, "]");

    return bRes;
}

bool ChannelChecker::readIdx(StRef<String> const dir_name, StRef<String> const channel_name)
{
    TimeChecker tc;tc.Start();

    bool bRes = false;

    StRef<String> const idx_file = st_makeString (dir_name, "/", channel_name, "/", channel_name, ".idx");
    std::string path = idx_file->cstr();
    std::ifstream idxFile;
    idxFile.open(path);

    std::string delimiter = "|";
    if (idxFile.is_open())
    {
        std::string line;
        while ( std::getline (idxFile, line) )
        {
            StRef<String> strToken;
            int initTime = 0;
            int endTime = 0;
            size_t pos = 0;

            pos = line.rfind(delimiter);
            strToken = st_makeString(line.substr(pos + delimiter.length()).c_str());
            strToInt32_safe(strToken->cstr(), &endTime);
            line.erase(pos);

            pos = line.rfind(delimiter);
            strToken = st_makeString(line.substr(pos + delimiter.length()).c_str());
            strToInt32_safe(strToken->cstr(), &initTime);
            line.erase(pos);

            ChChDiskTimes chChDiskTimes;
            chChDiskTimes.times.timeStart = initTime;
            chChDiskTimes.times.timeEnd = endTime;
            chChDiskTimes.diskName = std::string(dir_name->cstr());

            m_chFileDiskTimes[line] = chChDiskTimes;
        }
        idxFile.close();
        logD(channelcheck, _func_, "read successful, idxfile: ", path.c_str());
        bRes = true;
    }
    else
    {
        logD(channelcheck, _func_, "fail to open idxFile: ", path.c_str());
        bRes = false;
    }

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_, "NvrData.writeIdx exectime = [", t, "]");

    return bRes;
}

ChannelChecker::ChannelFileTimes
ChannelChecker::getChannelExistence()
{
    logD(channelcheck, _func_,"getChannelExistence");

    TimeChecker tc;tc.Start();

    cleanCache();
    updateCache(true);

    m_mutex.lock();

    ChannelFileTimes chFileTimes;
    ChannelFileDiskTimes::iterator it;
    chFileTimes.reserve(m_chFileDiskTimes.size());
    for(it = m_chFileDiskTimes.begin(); it != m_chFileDiskTimes.end(); ++it)
    {
        ChChTimes chChTimes;
        chChTimes.timeStart = (*it).second.times.timeStart;
        chChTimes.timeEnd = (*it).second.times.timeEnd;
        chFileTimes.push_back(chChTimes);
    }


    if(chFileTimes.size() >= 2)
    {
        ChannelFileTimes::iterator it = chFileTimes.begin();
        std::advance (it,1);

        while(1)
        {
            if (it == chFileTimes.end())
                break;

            std::vector<ChChTimes>::iterator it1 = it;
            std::advance (it1,-1);
            if ((it->timeStart - (it1)->timeEnd) < CONCAT_INTERVAL)
            {
                ChChTimes chChTimes;
                chChTimes.timeStart = (it1)->timeStart;
                chChTimes.timeEnd = (it)->timeEnd;

                std::vector<ChChTimes>::iterator prev = it;
                std::advance (prev,-1);
                std::vector<ChChTimes>::iterator prevPrev = it;
                std::advance (prevPrev,-2);

                chFileTimes.erase(it);
                chFileTimes.erase(prev);
                std::vector<ChChTimes>::iterator prevPrev1 = prevPrev;
                std::advance (prevPrev1,1);
                chFileTimes.insert(prevPrev1, chChTimes);
                it = chFileTimes.begin();
                std::advance (it,1);
            }
            else ++it;
        }
    }

    m_mutex.unlock();

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"getChannelExistence exectime = [", t, "]");

    return chFileTimes;
}

ChannelChecker::ChannelFileDiskTimes
ChannelChecker::getChannelFileDiskTimes ()
{
    logD(channelcheck, _func_,"getChannelFileDiskTimes");

    TimeChecker tc;tc.Start();

    cleanCache();
    updateCache(true);

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"getChannelFileDiskTimes exectime = [", t, "]");

    return m_chFileDiskTimes;
}

ChannelChecker::DiskSizes
ChannelChecker::getDiskSizes ()
{
    DiskSizes diskSizes;

    std::string curPath = m_recpathConfig->GetNextPath();
    while(curPath.length() != 0)
    {
        Uint64 size = 0;

        StRef<String> strRecDir = st_makeString(curPath.c_str());
        Ref<Vfs> const vfs = Vfs::createDefaultLocalVfs (strRecDir->mem());
        NvrFileIterator file_iter;
        file_iter.init (vfs, m_channel_name->mem(), 0);
        StRef<String> path = file_iter.getNext();
        if(path != NULL && !path->isNullString()) // if <strRecDir>/<channel_name> folder exist (and it is non empty)
        {
#ifdef __linux__
            StRef<String> fullRecDir;
            char ch = *curPath.rbegin();
            if(ch != '/')
                fullRecDir = st_makeString(curPath.c_str(), "/", m_channel_name, "/");
            else
                fullRecDir = st_makeString(curPath.c_str(), "/", m_channel_name);
            size = get_dir_space(fullRecDir->cstr()); // in bytes
#else
            size = 0; // TODO: implement
#endif
        }
        else
        {
            size = 0;
        }

        diskSizes[curPath] = size;

        curPath = m_recpathConfig->GetNextPath(curPath);
    }

    return diskSizes;
}

ChannelChecker::ChannelFileSummary
ChannelChecker::getChFileSummary(const std::string & dirname)
{
    logD(channelcheck, _func_,"getChFileSummary");

    ChannelFileSummary chFileSummary;
    for(ChannelFileDiskTimes::iterator itr = m_chFileDiskTimes.begin(); itr != m_chFileDiskTimes.end(); itr++)
    {
        if(itr->second.diskName.compare(dirname) == 0)
        {
            ChChTimes chChTimes;
            chChTimes.timeStart = itr->second.times.timeStart;
            chChTimes.timeEnd = itr->second.times.timeEnd;
            chFileSummary[itr->first] = chChTimes;
        }
    }

    return chFileSummary;
}

ChannelChecker::CheckResult
ChannelChecker::initCache()
{
    logD(channelcheck, _func_,"channel_name: [", m_channel_name, "]");

    TimeChecker tc;tc.Start();

    m_mutex.lock();

    initCacheFromIdx();
    completeCache();

    m_mutex.unlock();

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"ChannelChecker.initCache exectime = [", t, "]");

    return CheckResult_Success;
}

ChannelChecker::CheckResult
ChannelChecker::initCacheFromIdx()
{
    logD(channelcheck, _func_,"channel_name: [", m_channel_name, "]");

    TimeChecker tc;tc.Start();

    std::string curPath = m_recpathConfig->GetNextPath();
    while(curPath.length() != 0)
    {
        StRef<String> strRecDir = st_makeString(curPath.c_str());

        readIdx(strRecDir, m_channel_name);

        curPath = m_recpathConfig->GetNextPath(curPath);
    }

    if(!m_chFileDiskTimes.empty()) // if cache is not empty, then we add only new records
    {
        ChannelFileDiskTimes::reverse_iterator itr = m_chFileDiskTimes.rbegin();
        StRef<String> strLastfile = st_makeString(itr->first.c_str());
        StRef<String> strRecDir = st_makeString(itr->second.diskName.c_str());
        addRecordInCache(strLastfile, strRecDir, true);
    }

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"ChannelChecker.initCacheFromIdx exectime = [", t, "]");

    return CheckResult_Success;
}

bool
ChannelChecker::DeleteFromCache(const std::string & dirName, const std::string & fileName)
{
    logD(channelcheck, _func_,"filename: [", dirName.c_str(), "/", fileName.c_str(), "]");

    m_mutex.lock();

    m_chFileDiskTimes.erase(fileName);
    ChannelFileSummary chFileSummary = getChFileSummary(dirName);
    StRef<String> strRecDir = st_makeString(dirName.c_str());
    writeIdx(chFileSummary, strRecDir, m_channel_name);

    m_mutex.unlock();

    return true;
}

ChannelChecker::CheckResult
ChannelChecker::updateCache(bool bForceUpdate)
{
    CheckResult rez = CheckResult_Success;

    logD(channelcheck, _func_);

    m_mutex.lock();

    ChannelFileDiskTimes::reverse_iterator itr = m_chFileDiskTimes.rbegin();

    if(itr != m_chFileDiskTimes.rend()) // if last recorded file exists
    {
        std::string lastfile = itr->first;
        StRef<String> const strlastfile = st_makeString(lastfile.c_str());
        std::string lastdir = itr->second.diskName;
        StRef<String> const strlastdir = st_makeString(lastdir.c_str());

        Time timeOfRecord = 0;
        StRef<String> const flv_filename = st_makeString (lastfile.c_str(), ".flv");
        FileNameToUnixTimeStamp().Convert(flv_filename, timeOfRecord);
        timeOfRecord = timeOfRecord / 1000000000LL;

        bool bNewerFile = false; // false - last recorded file is recording file now, true - there is newer file
        std::string curPath = m_recpathConfig->GetNextPath();
        while(curPath.length() != 0)
        {
            NvrFileIterator file_iter;
            StRef<String> strRecDir = st_makeString(curPath.c_str());
            Ref<Vfs> const vfs = Vfs::createDefaultLocalVfs (strRecDir->mem());
            file_iter.init (vfs, m_channel_name->mem(), timeOfRecord);
            StRef<String> path = file_iter.getNext();
            // here path is last recorded file (if it is the same disk as for last recorded file)
            // or just some old last file (if it is not the same disk as for last recorded file)
            // or NULL (if current disk(curPath) doesnt contain files)
            // OR "" (if current disk(curPath) didnt contain files and now has one recording file)
            if(path != NULL)
            {
                StRef<String> pathNext = file_iter.getNext();
                // pathNext is new recording file
                // or NULL (if nvr writes on other disk or last recorded file is recording now)
                if(pathNext != NULL && !pathNext->isNullString())
                {
                    ChannelFileSummary files_existence;
                    // update duration for last recorded file
                    addRecordInCache(strlastfile, strlastdir, true);
                    files_existence = getChFileSummary(lastdir);
                    writeIdx(files_existence, strlastdir, m_channel_name);

                    // add new recording file in cache
                    addRecordInCache(pathNext, strRecDir, false);
                    files_existence = getChFileSummary(curPath);
                    writeIdx(files_existence, strRecDir, m_channel_name);

                    bNewerFile = true;
                    break; // recording file is only one for one source
                }
            }
            curPath = m_recpathConfig->GetNextPath(curPath);
        }
        if(!bNewerFile)
        {
            ChannelFileSummary files_existence;
            // update duration for last recorded file
            addRecordInCache(strlastfile, strlastdir, bForceUpdate);
            files_existence = getChFileSummary(lastdir);
            writeIdx(files_existence, strlastdir, m_channel_name);
        }
    }
    else // no one records exist
    {
        std::string curPath = m_recpathConfig->GetNextPath();
        while(curPath.length() != 0)
        {
            NvrFileIterator file_iter;
            StRef<String> strRecDir = st_makeString(curPath.c_str());
            Ref<Vfs> const vfs = Vfs::createDefaultLocalVfs (strRecDir->mem());
            file_iter.init (vfs, m_channel_name->mem(), 0);
            StRef<String> path = file_iter.getNext();
            if(path != NULL && !path->isNullString()) // if it is not null, then it is recording file
            {
                ChannelFileSummary files_existence;
                addRecordInCache(path, strRecDir, false);
                files_existence = getChFileSummary(curPath);
                writeIdx(files_existence, strRecDir, m_channel_name);
                break; // recording file is only one for one source
            }
            curPath = m_recpathConfig->GetNextPath(curPath);
        }
    }

    m_mutex.unlock();

    return rez;
}

ChannelChecker::CheckResult
ChannelChecker::completeCache()
{
    logD(channelcheck, _func_,"channel_name: [", m_channel_name, "]");

    TimeChecker tc;tc.Start();

    CheckResult rez = CheckResult_Success;
    std::string curPath = m_recpathConfig->GetNextPath();
    while(curPath.length() != 0)
    {
        Time timeOfRecord = 0;
        ChannelFileSummary files_existence = getChFileSummary(curPath);
        if(!files_existence.empty())
        {
            std::string lastfile = files_existence.rbegin()->first;
            StRef<String> const flv_filename = st_makeString (lastfile.c_str(), ".flv");
            FileNameToUnixTimeStamp().Convert(flv_filename, timeOfRecord);
            timeOfRecord = timeOfRecord / 1000000000LL;
        }

        NvrFileIterator file_iter;
        StRef<String> strRecDir = st_makeString(curPath.c_str());
        Ref<Vfs> const vfs = Vfs::createDefaultLocalVfs (strRecDir->mem());
        file_iter.init (vfs, m_channel_name->mem(), timeOfRecord);

        StRef<String> path = file_iter.getNext();
        while(path != NULL && !path->isNullString())
        {
            rez = addRecordInCache(path, strRecDir, true);
            path = file_iter.getNext();
        }

        files_existence = getChFileSummary(curPath);
        writeIdx(files_existence, strRecDir, m_channel_name);

        curPath = m_recpathConfig->GetNextPath(curPath);
    }

    Time t;tc.Stop(&t);
    logD (channelcheck, _func_,"ChannelChecker.completeCache exectime = [", t, "]");

    return rez;
}

ChannelChecker::CheckResult
ChannelChecker::cleanCache()
{
    logD(channelcheck, _func_,"channel_name: [", m_channel_name, "]");

    m_mutex.lock();

    if(m_chFileDiskTimes.empty())
    {
        m_mutex.unlock();
        return CheckResult_Success;
    }

    TimeChecker tc;tc.Start();

    std::string curPath = m_recpathConfig->GetNextPath();
    while(curPath.length() != 0)
    {
        StRef<String> strRecDir = st_makeString(curPath.c_str());
        ConstMemory record_dir = strRecDir->mem();
        Ref<Vfs> const vfs = Vfs::createDefaultLocalVfs (record_dir);

        NvrFileIterator file_iter;
        file_iter.init (vfs, m_channel_name->mem(), 0); // get the oldest file

        StRef<String> path = file_iter.getNext();
        if(path != NULL)
        {
            Time timeOfFirstFile = 0;
            {
                StRef<String> const flv_filename = st_makeString (path, ".flv"); // replace by read from idx ??????
                FileNameToUnixTimeStamp().Convert(flv_filename, timeOfFirstFile);
                timeOfFirstFile = timeOfFirstFile / 1000000000LL;
            }

            ChannelFileDiskTimes::iterator it = m_chFileDiskTimes.begin();
            while(it != m_chFileDiskTimes.end())
            {
                if(it->second.diskName.compare(curPath) == 0)
                {
                    logD(channelcheck, _func_,"file [", it->first.c_str(), ",{", it->second.times.timeStart, ",", it->second.times.timeEnd, "}]");
                    Time timeOfRecord = 0;
                    StRef<String> const flv_filename = st_makeString ((*it).first.c_str(), ".flv");
                    FileNameToUnixTimeStamp().Convert(flv_filename, timeOfRecord);
                    timeOfRecord = timeOfRecord / 1000000000LL;

                    logD(channelcheck, _func_,"timeOfFirstFile: [", timeOfFirstFile, "], timeOfRecord: [", timeOfRecord, "]");
                    if(timeOfRecord < timeOfFirstFile)
                    {
                        logD(channelcheck, _func_,"clean it!");
                        m_chFileDiskTimes.erase(it++);
                    }
                    else
                    {
                        logD(channelcheck, _func_,"all expired records have been deleted");
                        break;
                    }
                }
                else
                {
                    it++;
                }
            }
        }
        else
        {
            logD(channelcheck, _func_,"there is no files at all, clean all cache");

            ChannelFileDiskTimes::iterator itr = m_chFileDiskTimes.begin();
            while(itr != m_chFileDiskTimes.end())
            {
                if(itr->second.diskName.compare(curPath) == 0)
                {
                    m_chFileDiskTimes.erase(itr++);
                }
                else
                {
                    itr++;
                }
            }
        }

        ChannelFileSummary files_existence = getChFileSummary(curPath);
        writeIdx(files_existence, strRecDir, m_channel_name);

        curPath = m_recpathConfig->GetNextPath(curPath);
    }

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"ChannelChecker.cleanCache exectime = [", t, "]");

    m_mutex.unlock();

    return CheckResult_Success;
}

ChannelChecker::CheckResult
ChannelChecker::addRecordInCache(StRef<String> path, StRef<String> strRecDir, bool bUpdate)
{
    logD(channelcheck, _func_,"addRecordInCache:", path->cstr());

    TimeChecker tc;tc.Start();

    if(m_chFileDiskTimes.find(std::string(path->cstr())) == m_chFileDiskTimes.end() || bUpdate)
    {
        StRef<String> const flv_filename = st_makeString (path, ".flv");
        StRef<String> flv_filenameFull = st_makeString(strRecDir, "/", flv_filename);

        FileReader fileReader;
        fileReader.Init(flv_filenameFull);
        Time timeOfRecord = 0;

        FileNameToUnixTimeStamp().Convert(flv_filenameFull, timeOfRecord);
        int const unixtime_timestamp_start = timeOfRecord / 1000000000LL;
        int const unixtime_timestamp_end = unixtime_timestamp_start + (int)fileReader.GetDuration();
        logD(channelcheck, _func_,"(int)fileReader.GetDuration() = [", (int)fileReader.GetDuration(), "]");

        ChChDiskTimes chChDiskTimes;
        chChDiskTimes.times.timeStart = unixtime_timestamp_start;
        chChDiskTimes.times.timeEnd = unixtime_timestamp_end;
        chChDiskTimes.diskName = std::string(strRecDir->cstr());
        m_chFileDiskTimes[std::string(path->cstr())] = chChDiskTimes;
    }

    Time t;tc.Stop(&t);
    logD(channelcheck, _func_,"ChannelChecker.addRecordInCache exectime = [", t, "]");

    return CheckResult_Success;
}

void
ChannelChecker::refreshTimerTick (void * const _self)
{
    ChannelChecker * const self = static_cast <ChannelChecker*> (_self);

    logD (channelcheck, _func_);

    self->cleanCache();
    self->updateCache(false);
}

mt_const void
ChannelChecker::init (Timers * const mt_nonnull timers, RecpathConfig * recpathConfig, StRef<String> & channel_name)
{
    m_recpathConfig = recpathConfig;
    m_channel_name = channel_name;

    logD(channelcheck, _func_,"m_channel_name=", m_channel_name);

    initCache();
    dumpData();

    m_timers = timers;

    m_timer_key = m_timers->addTimer (CbDesc<Timers::TimerCallback> (refreshTimerTick, this, this),
                      TIMER_TICK,
                      true /* periodical */,
                      false /* auto_delete */);
}

void ChannelChecker::dumpData()
{
    logD(channelcheck, _func_,"m_channel_name = [", m_channel_name, "]");

    m_mutex.lock();

    logD(channelcheck, _func_, "File summary:");
    for(ChannelFileDiskTimes::iterator itr = m_chFileDiskTimes.begin(); itr != m_chFileDiskTimes.end(); ++itr)
    {
        logD(channelcheck, _func_,"file [", itr->first.c_str(), " on ", itr->second.diskName.c_str(), "] = {",
             itr->second.times.timeStart, ",", itr->second.times.timeEnd, "}");
    }

    m_mutex.unlock();
}

ChannelChecker::ChannelChecker(): m_timers(this),m_timer_key(NULL), m_recpathConfig(NULL)
{

}

ChannelChecker::~ChannelChecker ()
{
    if (m_timer_key) {
        m_timers->deleteTimer (this->m_timer_key);
        m_timer_key = NULL;
    }
    m_recpathConfig = NULL;
}

}
