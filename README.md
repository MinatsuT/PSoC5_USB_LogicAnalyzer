# PSoC5 16ch USB Logic Analyzer
This is an fx2lafw compatible firmware which can be used by [sigrok](https://sigrok.org/).

# Pin assignment

<pre>
PSoC5   Channel
P3_0 -- Ch0
  .
  .
  .
P3_7 -- Ch7

P0_0 -- Ch8
  .
  .
  .
P0_7 -- Ch15
GND  -- GND
</pre>

![Pin Assign](https://github.com/MinatsuT/PSoC5_USB_LogicAnalyzer/blob/master/PSoC5_LogicAnalyzer.png?raw=true)

# Limitations
Maximum capturing frequency is 250kHz. This limitation comes from bandwidth of USB Full Speed (12Mbps).
