"""Read nl80211 station counters; unsupported fields remain absent."""
import json,struct,subprocess,re
from nl_power import Power,attrs,attr
p=Power();status=subprocess.run(['wpa_cli','-i','wlan0','status'],capture_output=True,text=True,timeout=2).stdout
mac=re.search(r'^bssid=(.*)$',status,re.M).group(1)
r=p.request(p.family,17,p.interface+attr(6,bytes.fromhex(mac.replace(':',''))))
a=attrs(attrs(r[0][4:])[21]);out={'bssid':mac,'fields_present':list(a)}
for key,name in [(2,'rx_bytes32'),(3,'tx_bytes32'),(9,'rx_packets'),(10,'tx_packets'),(11,'tx_retries'),(12,'tx_failed')]:
 if key in a:out[name]=int.from_bytes(a[key],'little')
if 7 in a:out['signal_dbm']=struct.unpack('b',a[7])[0]
print(json.dumps(out))
