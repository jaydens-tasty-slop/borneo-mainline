#!/bin/sh
# Limits function tracing, and so what the pstore tracer records, to the code around an ADSP crash.
cd /sys/kernel/tracing || exit 1
echo 'qcom_swrm*' > set_ftrace_filter
for p in 'swrm_*' '*_macro_*' 'pm_clk*' 'rpm_*' '__pm_runtime*' 'wcd*' 'sdw_*' 'q6*' 'regmap_*' 'regcache_*' '_regmap*' 'lpi_*' 'snd_soc_*' 'dapm_*' 'soc_*' 'apr_*' 'qcom_glink*' 'rpmsg_*' 'qcom_smem_state*' 'qcom_smp2p*' 'smp2p*' 'rproc_*' 'qcom_q6v5*' 'qcom_pas*' 'qcom_scm*' '__scm*' 'clk_core_*' 'clk_prepare*' 'clk_enable*' 'clk_disable*' 'clk_unprepare*' 'pdr_*' 'snd_pcm_*' 'mbox_*' 'qcom_ssr*' 'arm_smmu_*' 'iommu_*' 'msm_*' 'qcom_apcs*'; do
	echo "$p" >> set_ftrace_filter 2>/dev/null
done
echo "filtered functions: $(wc -l < set_ftrace_filter)"
