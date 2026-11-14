"""回归 MSI 启动项清理的调度契约，仅解析生成的 WiX，不安装或卸载程序。"""

import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from build_integrated_installer import WIX, add_startup_cleanup


class StartupCleanupTests(unittest.TestCase):
    def setUp(self):
        """单独生成清理片段，避免构建安装包、读取现场配置或调用清理 DLL。"""
        self.product = ET.Element(f"{{{WIX}}}Product")
        add_startup_cleanup(self.product, Path("isolated-cleanup.dll"))

    def test_native_action_is_embedded_and_elevated(self):
        """MSI Binary 持有 DLL；卸载移除程序文件后仍可执行，并逐用户处理。"""
        binary = self.product.find(f"{{{WIX}}}Binary")
        self.assertEqual(binary.get("SourceFile"), "isolated-cleanup.dll")
        actions = self.product.findall(f"{{{WIX}}}CustomAction")
        self.assertEqual(len(actions), 2)
        for action in actions:
            self.assertEqual(action.get("BinaryKey"), binary.get("Id"))
            self.assertEqual(action.get("DllEntry"), "CleanDeskflowAutoStart")
            self.assertEqual(action.get("Impersonate"), "no")
            self.assertEqual(action.get("Return"), "ignore")

    def test_uninstall_upgrade_and_rollback_contract(self):
        """最终卸载才清理；正常事务提交后运行，禁回滚时只走互斥的延迟分支。"""
        scheduled = self.product.findall(f"{{{WIX}}}InstallExecuteSequence/{{{WIX}}}Custom")
        actions = {node.get("Id"): node for node in self.product.findall(f"{{{WIX}}}CustomAction")}
        properties = {node.get("Id"): node for node in self.product.findall(f"{{{WIX}}}SetProperty")}
        self.assertEqual(len(scheduled), 2)
        for node in scheduled:
            action = node.get("Action")
            condition = node.text
            self.assertIn('Installed AND REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE', condition)
            self.assertEqual(node.get("Before"), "InstallFinalize")
            self.assertEqual(properties[action].get("Before"), action)
            self.assertEqual(properties[action].get("Value"), "[INSTALLFOLDER]deskflow.exe")
            self.assertEqual(properties[action].text, condition)
            expected = "commit" if "NOT RollbackDisabled" in condition else "deferred"
            self.assertEqual(actions[action].get("Execute"), expected)


if __name__ == "__main__":
    unittest.main()
