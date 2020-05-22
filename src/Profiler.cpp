/**
 * @file Profiler.cpp
 * @author Thomas Gillis and Denis-Gabriel Caprace
 * @copyright Copyright © UCLouvain 2020
 * 
 * FLUPS is a Fourier-based Library of Unbounded Poisson Solvers.
 * 
 * Copyright <2020> <Université catholique de Louvain (UCLouvain), Belgique>
 * 
 * List of the contributors to the development of FLUPS, Description and complete License: see LICENSE and NOTICE files.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

#include "Profiler.hpp"


/**
 * @brief Construct a new Timer Agent
 * 
 * @param name the name
 */
TimerAgent::TimerAgent(string name){
    _name = name;
}

/**
 * @brief start the timer
 * 
 */
void TimerAgent::start() {
    _count += 1;
    _t0 = MPI_Wtime();
}

/**
 * @brief stop the timer
 * 
 */
void TimerAgent::stop() {
    // get the time
    _t1 = MPI_Wtime();
    // store it
    double dt = _t1 - _t0;
    _timeAcc  = _timeAcc + dt;
    _timeMax  = max(_timeMax, dt);
    _timeMin  = min(_timeMax, dt);
}

/**
 * @brief reset the timer
 * 
 */
void TimerAgent::reset() {
    _t1      = 0.0;
    _t0      = 0.0;
    _timeAcc = 0.0;
    _timeMax = 0.0;
    _timeMin = 0.0;
}

/**
 * @brief adds memory to the timer to compute bandwith
 * 
 */
void TimerAgent::addMem(size_t mem){
    _memsize += mem;
}

/**
 * @brief add a child to the timer 
 * 
 * @param child 
 */
void TimerAgent::addChild(TimerAgent* child) {
    string childName = child->name();
    map<string, TimerAgent*>::iterator it = _children.find(childName);
    // if it does not already exist
    if (it == _children.end()) {
        _children[childName] = child;
        child->setDaddy(this);
    }
}
/**
 * @brief store the dady pointer
 * 
 * @param daddy 
 */
void TimerAgent::setDaddy(TimerAgent* daddy) {
    _daddy  = daddy;
    _isroot = false;
}

/**
 * @brief display the time accumulated. If it's a ghost timer (no calls), we sum the time of the children
 * 
 * @return double 
 */
double TimerAgent::timeAcc() const {
    if (_count > 0) {
        return _timeAcc;
    } else {
        double sum = 0.0;
        for (map<string,TimerAgent*>::const_iterator it = _children.begin(); it != _children.end(); it++) {
            const TimerAgent* child = it->second;
            sum += child->timeAcc();
        }
        return sum;
    }
}

/**
 * @brief display the min time among all calls. If it's a ghost timer (no calls), we sum the time of the children
 * 
 * @return double 
 */
double TimerAgent::timeMin() const {
    if (_count > 0) {
        return _timeMin;
    } else {
        double sum = 0.0;
        for (map<string,TimerAgent*>::const_iterator it = _children.begin(); it != _children.end(); it++) {
            const TimerAgent* child = it->second;
            sum += child->timeMin();
        }
        return sum;
    }
}

/**
 * @brief display the max time among all calls. If it's a ghost timer (no calls), we sum the time of the children
 * 
 * @return double 
 */
double TimerAgent::timeMax() const {
    if (_count > 0) {
        return _timeMax;
    } else {
        double sum = 0.0;
        for (map<string,TimerAgent*>::const_iterator it = _children.begin(); it != _children.end(); it++) {
            const TimerAgent* child = it->second;
            sum += child->timeMax();
        }
        return sum;
    }
}

void TimerAgent::writeParentality(FILE* file, const int level){
    fprintf(file,"%d;%s",level,_name.c_str());
    for (map<string, TimerAgent*>::const_iterator it = _children.begin(); it != _children.end(); it++) {
        const TimerAgent* child = it->second;
        string childName = child->name();
        fprintf(file,";%s",childName.c_str());
    }
    fprintf(file,"\n");

    for (map<string, TimerAgent*>::const_iterator it = _children.begin(); it != _children.end(); it++) {
        it->second->writeParentality(file,level+1);
    }
}

/**
 * @brief display the time for the TimerAgent
 * 
 * @param file 
 * @param level 
 * @param totalTime 
 */
void TimerAgent::disp(FILE* file,const int level, const double totalTime){
    
    // check if any proc has called the agent
    int totalCount;
    MPI_Allreduce(&_count,&totalCount,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
    // if someone has every call the agent, display it
    if (totalCount > 0) {
        // get the size and usefull stuffs
        int commSize, rank;
        MPI_Comm_size(MPI_COMM_WORLD, &commSize);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        double scale = 1.0/commSize;

        // compute the counters (mean, max, min)
        double localCount = _count;
        double meanCount, maxCount, minCount;
        MPI_Allreduce(&localCount, &meanCount, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&localCount, &maxCount, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&localCount, &minCount, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
        meanCount *= scale;

        // compute times passed inside + children
        double localTime = _timeAcc;
        double meanTime, maxTime, minTime;
        MPI_Allreduce(&localTime, &meanTime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&localTime, &minTime, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
        MPI_Allreduce(&localTime, &maxTime, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        meanTime *= scale;
        
        double meanTimePerCount = meanTime/meanCount;
        double minTimePerCount = minTime/meanCount;
        double maxTimePerCount = maxTime/meanCount;
        double glob_percent = meanTime/totalTime*100.0;

        // compute the self time  = time passed inside - children
        double sumChild = 0.0;
        for (map<string,TimerAgent*>::iterator it = _children.begin(); it != _children.end(); it++) {
            TimerAgent* child = it->second;
            sumChild += child->timeAcc();
        }
        double locSelfTime = (this->timeAcc()-sumChild);
        double selfTime;
        double self_percent;
        FLUPS_CHECK(locSelfTime >= 0.0,"The timer %s does not include his children",_name.c_str(), LOCATION);
        MPI_Allreduce(&locSelfTime, &selfTime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        selfTime *= scale;
        self_percent = selfTime / totalTime * 100.0;

        // comnpute the time passed inside the daddy
        double loc_percent;
        if (_daddy != NULL) {
            double dadLocalTime = _daddy->timeAcc();
            double dadMeanTime;
            MPI_Allreduce(&dadLocalTime, &dadMeanTime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
            dadMeanTime *= scale;
            loc_percent = meanTime / dadMeanTime * 100.0;
            
        } else {
            loc_percent = 100.0;
        }

        // compute the bandwith
        double localBandTime = _timeAcc;
        double localBandMemsize = (double) _memsize;
        double bandMemsize, bandTime, meanBandwidth;
        MPI_Allreduce(&localBandTime, &bandTime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(&localBandMemsize, &bandMemsize, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        meanBandwidth =(bandMemsize/bandTime)/std::pow(10.0,6.0);

        // setup the displayed name
        string myname = _name;
        if (level > 1) {
            myname = " " + myname;
        }
        for (int l = 1; l < level; l++) {
            myname = "--" + myname;
        }

        // printf the important information
        if (rank == 0) {
            printf("%-25.25s|  %9.4f\t%9.4f\t%9.6f\t%9.6f\t%9.6f\t%9.6f\t%9.6f\t%09.1f\t%9.2f\n", myname.c_str(), glob_percent, loc_percent, meanTime, selfTime, meanTimePerCount, minTimePerCount, maxTimePerCount, meanCount,meanBandwidth);
            if (file != NULL) {
                fprintf(file, "%s;%09.6f;%09.6f;%09.6f;%09.6f;%09.6f;%09.6f;%09.6f;%09.0f;%09.2f\n", _name.c_str(), glob_percent, loc_percent, meanTime, selfTime, meanTimePerCount, minTimePerCount, maxTimePerCount, meanCount,meanBandwidth);
            }
        }
    }
    // recursive call to the childrens
    for (map<string,TimerAgent*>::iterator it = _children.begin(); it != _children.end(); it++) {
        TimerAgent* child = it->second;
        child->disp(file,level+1,totalTime);
    }
}

//===============================================================================================================================
//===============================================================================================================================
//===============================================================================================================================


Profiler::Profiler(): _name("default")
{
    _createSingle("root");
}
Profiler::Profiler(const string myname): _name(myname)
{
    _createSingle("root");
}
Profiler::~Profiler() {
    for (map<string, TimerAgent*>::iterator it = _timeMap.begin(); it != _timeMap.end(); it++) {
        TimerAgent* current = it->second;
        delete(current);
    }
}

/**
 * @brief create a TimerAgent inside the #_timeMap
 * 
 * @param name the key of the entry
 */
void Profiler::_createSingle(string name) {
    map<string, TimerAgent*>::iterator it = _timeMap.find(name);
    // if it does not already exist
    if (it != _timeMap.end()){
        _timeMap[name]->reset();
    }
    else if (it == _timeMap.end()) {
        _timeMap[name] = new TimerAgent(name);
        _timeMap[name]->reset();
    }
}

/**
 * @brief create a new TimerAgent with "root" as parent
 * 
 * @param name the TimerAgent name
 */
void Profiler::create(string name) {
    create(name,"root");
}

/**
 * @brief create a new TimerAgent 
 * 
 * @param child the new TimerAgent
 * @param daddy the dad of the new TimerAgent if it does not exists, it is created
 */
void Profiler::create(string child, string daddy) {
    // create a new guy
    _createSingle(child);
    // find the daddy agent in the root
    map<string, TimerAgent*>::iterator it = _timeMap.find(daddy);
    if(it == _timeMap.end()){
        create(daddy);
    }
    _timeMap[daddy]->addChild(_timeMap[child]);
}

/**
 * @brief start the timer of the TimerAgent
 * 
 * @param name the TimerAgent name
 */
void Profiler::start(string name) {
#ifdef NDEBUG
    _timeMap[name]->start();
#else
    map<string, TimerAgent*>::iterator it = _timeMap.find(name);
    if (it != _timeMap.end()) {
        _timeMap[name]->start();
    }
    else{
        string msg = "timer "+name+ " not found";
        FLUPS_ERROR(msg, LOCATION);
    }
#endif
}

/**
 * @brief stop the timer of the TimerAgent
 * 
 * @param name the TimerAgent name
 */
void Profiler::stop(string name) {
#ifdef NDEBUG
    _timeMap[name]->stop();
#else
    map<string, TimerAgent*>::iterator it = _timeMap.find(name);
    if (it != _timeMap.end()) {
        _timeMap[name]->stop();
    }
    else{
        string msg = "timer "+name+ " not found";
        FLUPS_ERROR(msg, LOCATION);
    }
#endif
}

void Profiler::addMem(string name,size_t mem) {
#ifdef NDEBUG
    _timeMap[name]->addMem(mem);
#else
    map<string, TimerAgent*>::iterator it = _timeMap.find(name);
    if (it != _timeMap.end()) {
        _timeMap[name]->addMem(mem);
    }
    else{
        string msg = "timer "+name+ " not found";
        FLUPS_ERROR(msg, LOCATION);
    }
#endif
}

/**
 * @brief get the accumulated time
 * 
 * @param name 
 * @return double 
 */
double Profiler::get_timeAcc(const std::string ref){

    int commSize;
    double localTotalTime = _timeMap[ref]->timeAcc();
    double totalTime;
    MPI_Comm_size(MPI_COMM_WORLD, &commSize);

    MPI_Allreduce(&localTotalTime, &totalTime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    totalTime /= commSize;

    return totalTime;
}


/**
 * @brief display the whole profiler using 
 * 
 */
void Profiler::disp() {
   this->disp("root");
}
/**
 * @brief display the profiler using the timer spent in ref as a reference for the global percentage computation
 * 
 * @param ref 
 */
void Profiler::disp(const std::string ref) {    
    int commSize, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &commSize);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    //-------------------------------------------------------------------------
    /** - I/O of the parentality */
    //-------------------------------------------------------------------------
    FILE* file;
    string folder = "./prof";

    if (rank == 0) {
        struct stat st = {0};
        if (stat(folder.c_str(), &st) == -1) {
                mkdir(folder.c_str(), 0770);
        }

        string filename = folder + "/" + _name + "_parent.csv";
        file            = fopen(filename.c_str(), "w+");

        if (file != NULL) {
            _timeMap["root"]->writeParentality(file,0);
            fclose(file);
        } else {
            printf("unable to open file %s !", filename.c_str());
        }
    }
    

    //-------------------------------------------------------------------------
    /** - do the IO of the timing */
    //-------------------------------------------------------------------------
    
    if (rank == 0) {
        string filename = "./prof/" + _name + "_time.csv";
        file            = fopen(filename.c_str(), "w+");
    }
    // display the header
    if (rank == 0) {
        printf("===================================================================================================================================================\n");
        printf("        PROFILER %s \n", _name.c_str());
        // printf("\t-NAME-   \t\t\t-%% global-\t-%% local-\t-Total time-\t-Self time-\t-time/call-\t-Min tot time-\t-Max tot time-\t-Mean cnt-\n");
        printf("%25s|  %-13s\t%-13s\t%-13s\t%-13s\t%-13s\t%-13s\t%-13s\t%-13s\t%-13s\n","-NAME-    ", "-% global-", "-% local-", "-Total time-", "-Self time-", "-time/call-", "-Min time-", "-Max time-","-Mean cnt-","-(MB/s)-");
    }
    // get the global timing
    double totalTime = this->get_timeAcc(ref);

    // display root with the total time
    _timeMap["root"]->disp(file,0,totalTime);
    // display footer
    if (rank == 0) {
        printf("===================================================================================================================================================\n");
        printf("%% global - %% of the total time passed inside or in its children (based on the mean time among processors\n");
        // printf("%% self glob - %% of the total time passed inside (children not included, based on the mean time among processors\n");
        printf("%% local - %% of the dad's time passed inside or in its children (from the mean time among processors\n");
        printf("Total time - the total time spend in that timer (averaged among the processors)\n");
        printf("Self time - the self time spend in that timer = children not included (averaged among the processors)\n");
        printf("Time/call - the total time spend in that timer per call of the timer (averaged among the processors)\n");
        printf("Min time - the min time / call spend in that timer among the processors\n");
        printf("Max time - the max time / call spend in that timer among the processors\n");
        printf("Mean cnt - the total number of time the timer has been called (averaged among the processors)\n");
        printf("===================================================================================================================================================\n");
    
        if (file != NULL) {
            fclose(file);
        } else {
            printf("unable to open file for profiling !");
        }
    }
}
