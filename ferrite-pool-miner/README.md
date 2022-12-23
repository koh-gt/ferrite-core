Ferrite GPU/ASIC stratum pool mining kit for Windows

---

Quick setup:
Open ccminer.conf and enter your wallet address here.
"user" : "YOUR_WALLET_ADDRESS_HERE",
Save the .conf file.

Run ccminer-x64.exe

To check your pool mining balance and hashrate: 
https://mine.altcoinbuilders.com/?address=YOUR_WALLET_ADDRESS_HERE

Payouts are per block for the default stratum-mining pool. However, the fee is very high >25%.

---

By Default as of version 1.2.2 set to GPU mining, mining-coins.com mining pool.

Notes:
"intensity" : 22.0,
Intensity setting from 1.0 to 25.0. 
Higher intensity allows for higher hashrates, 
although not necessarily so if the system is overwhelmed.

"launch-config" : "24x16"
24x16 if your GPU only has less than 4GB of memory.
32x16 if your GPU has more memory.

---

As of 24 Dec 2022, the network hashrate is about 100 MH/s. 
This means that a hashrate of 500 kH/s will mine an average of 1 block every 3 hours solo.
Since you are in a pool, even if the hashrate is low, you will receive your share of consistent rewards every interval.
The difficulty will increase or decrease depending on hashrate which will affect block mining rates.


The current block reward now (block 24300) is 100 FEC which will halve every 301107 blocks.
Halving will take place approximately once every 8 months.
There will only be 60,221,400 Ferrite coins in circulation.
