"""Compare dumped bilinear114 inputs with independently prepared OpenCV pixels."""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--manifest', type=Path, required=True)
p.add_argument('--results', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
rows = []
for case in json.loads(a.manifest.read_text())['cases']:
    result = json.loads((a.results / (case['id']+'.json')).read_text())
    rw, rh = result['resized']
    image = cv2.cvtColor(cv2.imread(case['image']), cv2.COLOR_BGR2RGB)
    expected = np.full((640,640,3), 114, np.uint8)
    expected[(640-rh)//2:(640-rh)//2+rh, (640-rw)//2:(640-rw)//2+rw] = cv2.resize(image, (rw,rh), interpolation=cv2.INTER_LINEAR)
    actual = np.fromfile(str(a.results / case['id'])+'.input.f32', np.float32).reshape(3,640,640).transpose(1,2,0)*255
    delta = np.abs(actual-expected)
    rows.append(dict(id=case['id'], max_byte_error=float(delta.max()), mean_byte_error=float(delta.mean())))
    if delta.max() > 1.0001:
        raise ValueError('preprocessing pixel mismatch: '+case['id'])
a.output.write_text(json.dumps(rows,indent=2)+'\n')
print('Input comparison:', len(rows), 'samples, at most one byte level difference')
