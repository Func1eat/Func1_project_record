 FTL 技术说明（hw/femu/bbssd/ftl.c）

  - 核心职责：实现 FEMU BlackBox SSD 的 FTL，包括参数初始化、地址映射、缓存命中、写指针推进、FDP/RU 分区管理、GC/磨损均衡、延迟建模和后台线程驱动。
  - 主要数据结构：ssdparams（几何/时延/策略参数），nand_*（页/块/面/Die/通道），maptbl/back_maptbl（主备 LPN→PPA 映射），rmap（PPA→LPN 反向映射），line_mgmt
    与 ru/fdp_ru_mgmt（行与恢复单元管理），LRUCache（读写缓存，基于双向链表+数组直址表），热度/统计表（读写热度、GC 计数、状态机计数等）。
  - 初始化流程：ssd_init() 依次设置参数 ssd_init_params()，构建 LRU 缓冲、NAND 拓扑、映射/反向映射表、GC 计数表，初始化行管理与写指针，构建 FDP RU 管理与 RUH
    表，分配热度/统计数组，启动 ftl_thread。
  - 读路径：ssd_read() 先查读缓存与备份映射；缺页时必要时触发 GC 并预写 ssd_pre_read()；根据 RU 模式选择 SLC/QLC 读命令，累积读热度并可能触发迁移（依据
    read_migration 策略与页面类型/延迟）。
  - GC 与磨损均衡：基于行级与 RU 级阈值 should_gc*/should_fdp_gc* 触发；do_fdp_gc() 选取 SLC/QLC victim RU（综合热度、无效页、区域模式等），迁移有效数据后擦
    价值。
  - 线程与调度：ftl_thread() 持续从 to_ftl 队列取请求，调用读写路径并将完成延迟回传 to_poller；定期更新统计/热度（代码中留有时间窗口计数与阈值调整逻辑）。
  - 关键常量/参数入口：所有默认几何与阈值从 bb_params 注入；GC 阈值、耐久度（SLC/QLC）、缓冲容量、读写模式、迁移开关、动态阈值等均在 ssd_init_params() 中
    读取。
  - 注意事项/风险点：LRU 直址表大小硬编码为 7,864,320，几何变化可能越界；ssd_write_flush() 按固定 RG_DEGREE 弹出节点，缓存不足会解链错误；读缓存命中未更新
    maxlat 可能产生零延迟返回。