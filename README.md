# PSoC5_USB_LogicAnalyzer
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

# Limitations
Maximum capturing frequency is 250kHz. This limitation comes from bandwidth of USB Full Speed (12Mbps).
