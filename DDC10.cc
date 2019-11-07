#include "DDC10.hh"
#include <iostream>
#include <tcl8.6/expect.h>
#include <sys/wait.h>
#include <math.h>
#include <string>


// Note: Need to make sure that you have tcl8.6 and expect libraries for communication with the DDC10 device
// The following below will be probably refactored for XENONnT 
DDC10::DDC10(){
   fHopts.signal_threshold = fHopts.sign = fHopts.rise_time_cut = fHopts.inner_ring_factor = fHopts.outer_ring_factor =-1;
   fHopts.integration_threshold = fHopts.parameter_0 = fHopts.parameter_1 = fHopts.parameter_2 = fHopts.parameter_3 = -1;
   fHopts.window = fHopts.prescaling = fHopts.component_status = fHopts.width_cut = fHopts.delay = -1;
   fHopts.address = fHopts.required = "";

}

DDC10::~DDC10(){
}

int DDC10::Initialize(HEVOptions d_opts)
{       
	fHopts = d_opts;
	// Prevent output of spawned process
	exp_loguser = 0;
        
	// Connect to DDC10 via ssh
        std::string temp = "ssh root@";
        temp += d_opts.address; 

        // exp_open spawn ssh and returns a stream.
        FILE *expect = exp_popen((char *) temp.c_str());
        if (expect == 0) return 1;

        enum { usage, permission_denied, command_not_found,
               command_failed, connection_failed, prompt };
      
        switch (exp_fexpectl(expect,
                exp_glob, "password:", prompt,
		exp_glob, "Network is unreachable", connection_failed,
                exp_end)) {
	case connection_failed:
		cout << endl << "DDC10: connection failed" << endl;
		return 1;	
        case prompt:
                break;
        case EXP_TIMEOUT:
                cout << "DDC10: Timeout,  may be invalid host" << endl;
                return 1;
        }

        fprintf(expect, "uClinux\r"); // Password for ssh connection to DDC-10
        switch (exp_fexpectl(expect,
                exp_glob, "root:~>", prompt,
                exp_glob, "Permission denied", permission_denied,
                exp_end)) {
        case prompt:
                break;
        case permission_denied:
                cout << endl << "DDC10: Permission denied" << endl;
                return 1;
        case EXP_TIMEOUT:
                cout << "DDC10: Timeout,  may be invalid host" << endl;
                return 1;
        }

        // Recalculate few values to fit to DDC10s' format
        bool success = true;
        long pars[4];
        pars[0] = round(d_opts.parameter_0 * pow(2,48));
        pars[1] = round(d_opts.parameter_1 * pow(2,48));
        pars[2] = round(d_opts.parameter_2 * pow(2,48));
        pars[3] = round(d_opts.parameter_3 * pow(2,48));

        int ParLow_int[4]; //int type needed for DDC10s' blackfin chip
        int ParHi_int[4];  //int type needed for DDC10s' blackfin chip

        for(int i=0; i<4; i++) {
                ParLow_int[i] = pars[i] & 0x00000000FFFFFFFF;
                ParHi_int[i] = pars[i] >> 32;
        }

        // Set Initialization command 
        char command [1000];
        sprintf(command, "./../HEveto/Initialize ");
        sprintf(command + strlen(command), "%d ",d_opts.sign);
        sprintf(command + strlen(command), "%d ",d_opts.window);
        sprintf(command + strlen(command), "%d ",d_opts.delay);
        sprintf(command + strlen(command), "%d ",d_opts.signal_threshold);
        sprintf(command + strlen(command), "%d ",d_opts.integration_threshold);
        sprintf(command + strlen(command), "%d ",d_opts.width_cut);
        sprintf(command + strlen(command), "%d ",d_opts.rise_time_cut);
        sprintf(command + strlen(command), "%d ",d_opts.component_status);

        sprintf(command + strlen(command), "%d ",ParHi_int[0]);
        sprintf(command + strlen(command), "%d ",ParLow_int[0]);
        sprintf(command + strlen(command), "%d ",ParHi_int[1]);
        sprintf(command + strlen(command), "%d ",ParLow_int[1]);
        sprintf(command + strlen(command), "%d ",ParHi_int[2]);
        sprintf(command + strlen(command), "%d ",ParLow_int[2]);
        sprintf(command + strlen(command), "%d ",ParHi_int[3]);
        sprintf(command + strlen(command), "%d ",ParLow_int[3]);

        sprintf(command + strlen(command), "%d ",d_opts.outer_ring_factor);
        sprintf(command + strlen(command), "%d ",d_opts.inner_ring_factor);
        sprintf(command + strlen(command), "%d ",d_opts.prescaling);
        
	// Send the Initialisation command
        fprintf(expect, "%s\r", command);

	// Check return of connection
	switch (exp_fexpectl(expect,
                exp_glob, "not found", command_not_found, // 1 case
                exp_glob, "wrong usage", usage, // another case
                exp_glob, "initialization done", prompt, // third case
                exp_end)) {
        case command_not_found:
                cout << endl << "DDC10: unknown command" << endl;
                success = false;
                break;
        case usage:
                success = false;
                cout << endl << "DDC10: wrong usage of \"Initialize\"" << endl;
                break;
        case EXP_TIMEOUT:
		success = false;
                cout << "DDC10: Login timeout" << endl;
                break;
        case prompt:
		break;
        default:
		success = false;
		cout << endl << "DDC10: unknown error" << endl;
		break;
	}

        // Close the ssh connection
	fclose(expect);
	waitpid(exp_pid, 0, 0);// wait for expect to terminate

	if (success) return 0;
	else return 1;
}
