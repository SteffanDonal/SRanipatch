Requires Python 3.10–3.14. Conversion uses the MNN 3.6.1 Python package; no external executable or repository build is needed.

From `assets/models`:

```powershell
python -m pip install -r converter/requirements.txt
python converter/convert.py "D:\Documents\Workspaces\SRanipal\SRanipal_steam\model\LPrediction"
```

Writes `converter/lip_model.mnn`; `--output PATH` overrides the destination. Reads `lip_opencl.params` directly without TVM or OpenCL. Uses the fixed recovered lip network and checks the output against the reference model, excluding UUID and converter version metadata, before replacing the destination.
