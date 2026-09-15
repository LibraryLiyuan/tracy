# Query 长数据改造：存储基础检查点

2026-09-15，基于1ce08c45，工作分支feature/JNTracy-GpuCatalog-DiskIndex。本检查点不是GPUCatalog长数据改造完成声明。

## 已实现并验证

- 新增有界压缩定长记录存储：Zstd/raw块、磁盘ordinal目录、随机/跨块读取、身份与内容校验；不整体加载目录。支持按调用方预算计账和取消。
- Query索引schema14，将CPU/GPU Zone、extra、Gfx/Relation和GPU Reference等固定记录sidecar压缩；代次独立命名并最后发布manifest。按值读取，避免块切换后引用失效。
- Native Scan逐帧序列使用压缩记录存储，保留逐签名checksum、帧号和原始数值；bundle发布、重开与策略重算接入新存储。
- Windows Query入口增加32 GiB进程提交内存限制；索引工作集辅助限制改为16 GiB。内存压力回收策略与启动Profile完整传递仍待完善。
- 添加8S/16S磁盘限额原语，Scan启动及缓存构建峰值开始使用Trace相对限制。加载索引与所有辅助产物的统一计账、最终8S发布门禁尚未完成。
- 修复GPU内存请求关联：过期的内层区间不再遮住仍有效的外层区间；用区间索引避免逐分配倒扫全部未来请求。此为独立正确性修正，不归入压缩收益。

## 校验

- 全部功能CTest：30/30通过，176.44秒；各新能力保留红/绿回归。
- 非Catalog既有测试Trace：349639120字节。新加载索引516204374字节，约1.4764倍；旧加载索引2120015128字节。CPU Zone仍为9854660条，CPU sidecar为105851142字节。已通过深校验和重开。
- 上述只衡量加载索引，不是整个Query的最终占用，也不是新30分钟Catalog录制的结果。未开始基线/新版单轮性能初评。
- 功能构建使用VS18 x64 Release /O2 /EHsc /bigobj、关闭LTO以降低迭代成本；正式性能比较仍需统一构建配置，不能混用结果。

## 输入阻断

独立30分钟stream的原生日志校验通过：746717条日志记录，Protocol90，完整。旧build-latest转换器为2026-08-25构建，慢速legacy尝试已在候选写出前终止并保留记录。

切换包含离线Catalog修复的工具后显著提速，但最后成功采样为61872/226710个client records（约27.3%）、提交内存约25 GiB，随后以exit5报告系统内存压力超限。未得到可用.tracy。转换器磁盘支撑属于新增范围，已向用户请求确认，尚未实施。

## 未完成

统一全部Query产物的8S/16S计账与发布门禁、完整启动资源Profile、Catalog原始表外置与质量权威迁移、所有定向查询与Scan迁移、GPU内存大JSON摘要拆表、真实输入转换及单轮初评。

详细执行计划及证据位于工作区reviews/query-longtrace-gpucatalog-20260914和GpuCatalog-DiskIndex；原始录制及历史分析保持不变。未合并DevBase、未推送、未替换正式工具。
