# redax Docs
D. Coderre, 3. January 2019

## Brief

This documentation should get you up and running with redax. At the time of writing the software is still 
in heavy development so if there are any issues with this documentation please file an issue on GitHub.

## Use Case

Redax is meant to be a full-scale DAQ solution facilitating readout of many channels in parallel. It is 
designed to be the production system for the XENONnT experiment. There is no 'standalone' or 'lite mode' 
included, so using this in a small-scale test setup could be overkill, since the system requires somewhat 
considerable infrastructure in order to function. However use in a long term, permanent lab setup, even 
if the number of channels is low, should be quite easy.

## Overview

This document provides setup steps in order, beginning with installation of prerequisites and ending in 
configuration of the final system. Use these links to navigate to the sub-pages.

  1. Installation of prerequisites
  2. Installing software and helper programs
  3. Configuration of backend databases
  4. DAQ Configuration options
  5. Examples of running the readout
  
A brief overview of the complete system follows. Please refer to Figure 1.

![Figure 1: A diagram of the complete system]({{ site.url }}/docs/figures/daq_software_overview.png)
