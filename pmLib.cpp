#include <string>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <fstream>
#include <unistd.h>
#include <future>
#include <thread>


#include <rocm_smi/rocm_smi.h>

// g++ -O3 -fPIC -shared pmLib.cpp -o pmLib.so -I /opt/rocm/include/ -lpthread -lrocm_smi64 -L /opt/rocm/rocm_smi/lib


namespace FRCouters {
long long int initValuesEnergy_pm[7];
long long int startTime;

// /sys/cray/pm_counters/memory_power
// 71 W 1700061089722572 us
// /sys/cray/pm_counters/memory_energy
// 81147367 J 1700061210920464 us

// SLURM_LOCALID == 0

std::future<void> poller;
bool terminated = false;

const char* base = "/sys/cray/pm_counters/";
const char* pathsPower[] = {
    "accel0_power",
    "accel2_power",
    "accel1_power",
    "accel3_power",
    "memory_power",
    "cpu_power",
    "power"
};

const char* pathsEnergy[] = {
    "accel0_energy",
    "accel1_energy",
    "accel2_energy",
    "accel3_energy",
    "memory_energy",
    "cpu_energy",
    "energy"
};

const char* tableHeaders[] = {
    "accel0_power",
    "accel1_power",
    "accel2_power",
    "accel3_power",
    "memory_power",
    "cpu_power",
    "power",
    "rsmi0_power",
    "rsmi1_power",
    "rsmi2_power",
    "rsmi3_power",
};


std::string envToString( const std::string & var ) {
     const char * val = std::getenv( var.c_str() );
     if ( val == nullptr ) { 
         return "";
     }
     else {
         return val;
     }
}


__attribute__((constructor)) void startPoller() {
    //

    if(envToString("SLURM_LOCALID") != "0") {
        return;
    }

    poller = std::async([&]() {

        rsmi_init(0);
        
        long long int maxValuesEnergy_pm[7];
        long long int prevValuesEnergy_pm[7];
        long long int currValuesEnergy_pm[7];
        long long int timestamps_pm[7];
        long long int prevTimestamps_pm[7];
        long long int prevTime;

        uint64_t rSmiPowerCounter[8];
		uint64_t rSmiEnergyCounterPrev[8];
		uint64_t rSmiTimestamps[8];
        float rSmiPowerComputed[8];

		float counter_resolution;
		uint64_t timestamp;

        for (int i = 0; i < 8; i++)
        {
            rsmi_dev_energy_count_get(i, &rSmiEnergyCounterPrev[i], &counter_resolution, &rSmiTimestamps[i]);
            rsmi_dev_power_ave_get(i, 0, &rSmiPowerCounter[i]);
        }
        
        std::string pid = std::to_string(getpid());

        std::string jobId = envToString("SLURM_JOBID");
        std::string nodeName = envToString("SLURMD_NODENAME");

        std::ofstream outTable("table_"+jobId+"_"+nodeName+".csv", std::fstream::out | std::fstream::app);
        outTable << "ts \t";
        for (const auto&pTh: tableHeaders) {
            outTable << pTh << "\t";
        }
        outTable << "\n";

        for (size_t i = 0; i < 6; i++)
        {
            maxValuesEnergy_pm[i] = 0;
            prevValuesEnergy_pm[i] = 0;
        }
        while (!terminated) {
            int i = 0;
            for (const auto&pTh: pathsPower) {
                auto p = std::string(pTh);
                std::ifstream counterFile(base + p);
                if (counterFile.good()) {
                    std::string counterLine;
                    std::getline(counterFile, counterLine);
                    long long int current = maxValuesEnergy_pm[i];
                    long long int maxVal = std::max(current, std::atoll(counterLine.substr(0, counterLine.find("W")).c_str()));
                    maxValuesEnergy_pm[i] = maxVal;
                    counterFile.close();
                }
                i++;
            }

            long long int newTime = std::chrono::steady_clock::now().time_since_epoch().count();

            i = 0;
            for (const auto&pTh: pathsEnergy) {
                auto p = std::string(pTh);
                std::ifstream counterFile(base + p);
                if (counterFile.good()) {
                    std::string counterLine;
                    std::getline(counterFile, counterLine);
                    currValuesEnergy_pm[i] = std::atoll(counterLine.substr(0, counterLine.find("J")).c_str());
                    timestamps_pm[i] = std::atoll(counterLine.substr(counterLine.find("J")+2, counterLine.find("us")).c_str());
                    counterFile.close();
                }
                i++;
            }
            i = 0;
            outTable << newTime << "\t";
            for (const auto&pTh: pathsEnergy) {
                long long int diff = currValuesEnergy_pm[i] - prevValuesEnergy_pm[i];
                long long int timeDiff = timestamps_pm[i] - prevTimestamps_pm[i];
                double timeDiffF = timeDiff*1E-6;
                outTable << (double)diff / (double)timeDiffF << "\t";
                prevValuesEnergy_pm[i] = currValuesEnergy_pm[i];
                prevTimestamps_pm[i] = timestamps_pm[i];
                i++;
            }
            prevTime = newTime;

            for (int i = 0; i < 8; i+=2)
            {
	        	uint64_t energyCounter;
    		    uint64_t timestamp;
                rsmi_dev_energy_count_get(i, &energyCounter, &counter_resolution, &timestamp);
                rsmi_dev_power_ave_get(i, 0, &rSmiPowerCounter[i]);
                float power = ((energyCounter-rSmiEnergyCounterPrev[i])*counter_resolution)/(float)(timestamp-rSmiTimestamps[i])*1000.f;

                rSmiPowerComputed[i] = power;
                rSmiTimestamps[i] = timestamp;
                rSmiEnergyCounterPrev[i] = energyCounter;
                outTable << std::to_string(power) << "\t";

            }
        
            outTable << "\n";


            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        outTable.close();
        int i;
        for (const auto&pTh: pathsEnergy) {
            auto p = std::string(pTh);
            std::ifstream counterFile(base + p, std::fstream::out | std::fstream::app);
            if (counterFile.good()) {
                std::cout << "max " << p << " " << maxValuesEnergy_pm[i] << " w" << std::endl;
            }
            i++;
        }
    });
    
}

__attribute__((destructor)) void stopPoller() {
    //
    if(envToString("SLURM_LOCALID") != "0") {
        return;
    }

    terminated = true;
    poller.get();
}



__attribute__((constructor)) void recordPmCounterStart() {
    if(envToString("SLURM_LOCALID") != "0") {
        return;
    }

    startTime = std::chrono::steady_clock::now().time_since_epoch().count();
    int i = 0;
    for (const auto&pTh: pathsEnergy) {
        auto p = std::string(pTh);
        std::ifstream counterFile(base + p);
        if (counterFile.good()) {
            std::string counterLine;
            std::getline(counterFile, counterLine);
            initValuesEnergy_pm[i] = std::atoll(counterLine.substr(0, counterLine.find("J")).c_str());
            counterFile.close();
        }
        i++;
    }
}

__attribute__((destructor)) void recordPmCounterEnd() {

    if(envToString("SLURM_LOCALID") != "0") {
        return;
    }
    std::string nodeName = envToString("SLURMD_NODENAME");

    float seconds = (std::chrono::steady_clock::now().time_since_epoch().count()-startTime)*1e-9f;
    int i = 0;
    for (const auto&pTh: pathsEnergy) {
        auto p = std::string(pTh);
        std::ifstream counterFile(base + p);
        if (counterFile.good()) {
            std::string counterLine;
            std::getline(counterFile, counterLine);
            long long int valNow = std::atof(counterLine.substr(0, counterLine.find("J")).c_str());
            long long int valStart = initValuesEnergy_pm[i];
            long long int diff = valNow - valStart;
            std::cout << nodeName <<  " " << seconds << " consumption " << p << " " << diff << " j average power " << diff/seconds << std::endl;
        }
        i++;
    }
}


}
